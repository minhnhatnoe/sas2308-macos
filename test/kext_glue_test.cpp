/*
 * kext_glue_test.cpp - the kext's IOKit-free half (kext/SAS2308Glue.cpp,
 * kext/mps_kext.cpp) on the real core and the IOC model, against a fake
 * IOSCSIParallelFamily with the family's contract (review TF1):
 *   - only tasks given to SetTimeoutForTask are timed (SCSIParallelTimer);
 *     their timeout calls HandleTimeout, nothing else does;
 *   - a task the HBA refused synchronously must not be on the timer list
 *     (CompleteParallelTask is the only call that removes it);
 *   - CompleteParallelTask once per accepted task, never after the task's
 *     target nub was destroyed;
 *   - thread calls with xnu semantics (an enter while the call runs starts a
 *     second, concurrent invocation: test/mps_host_os.h HostThreadCall).
 * Built with ASan/UBSan/LSan. KGLUE_ONLY=<id> runs one scenario.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mps_host_os.h"

static void Fail(const char *file, int line, const char *expr);
#define CHECK(c) do { if (!(c)) Fail(__FILE__, __LINE__, #c); } while (0)
#include "mock_ioc.h"

#define MPS_COMPAT_NO_LIBC_REMAP
extern "C" {
#include "../core/compat/mps_compat.h"
#include "../core/compat/cam_compat.h"
#include "../core/mps_mpi.h"
#include "../core/mpsvar.h"
#include "../core/mps_sas.h"
#include "../core/mps_xnu.h"
}
#include "../kext/SAS2308Glue.h"

static int gChecks;
static void Fail(const char *file, int line, const char *expr)
{
    printf("FAIL %s:%d: %s\n", file, line, expr);
    gHost.DumpTail();
    exit(1);
}
#undef CHECK
#define CHECK(c) do { gChecks++; if (!(c)) Fail(__FILE__, __LINE__, #c); } while (0)

/* the kext's xnu-only hooks, on the host */
extern "C" int mps_kext_os_init(void) { return 0; }
extern "C" void mps_kext_log(const struct mps_kext_host *, uint32_t cls, const char *line)
{
    gHost.log.push_back({cls, line});
    if (gHost.verbose)
        printf("    | [%s] %s\n", mps_xnu_category(cls), line);
}

/* ---- the fake family ------------------------------------------------------ */

struct FakeTask {
    uint32_t target = 0;
    uint64_t lun = 0, tag = 0;
    uint8_t cdb[16]{};
    uint8_t cdbLen = 10;
    int dir = MPSK_DIR_IN;
    uint64_t len = 4096;
    std::vector<uint8_t> td;
    uint64_t iova = 0;
    uint32_t timeoutMs = 30000;
    /* family state */
    bool outstanding = false, armed = false, mapped = false;
    uint64_t deadline = 0;
    int completions = 0, done = -1, response = -1;
    uint8_t scsiStatus = 0xFF;
    uint32_t destroysAtSubmit = 0;
};

struct SAS2308_HIDDEN Family : SAS2308GluePort {
    SAS2308Glue glue;
    MockIocModel *ioc = nullptr;
    struct mps_kext_host host{};
    struct mps_kext *k = nullptr;
    HostThreadCall calls[2];
    std::set<uint32_t> nubs;
    uint32_t creates[64]{}, destroys[64]{};
    std::function<void(uint32_t)> onCreate;
    bool ignoreTimeouts = false;            /* a family whose timer never fires */
    std::vector<FakeTask *> tasks;
    int timeouts = 0;

    bool TaskDescribe(void *t, TaskDesc *d) override {
        FakeTask *f = (FakeTask *)t;
        d->target = f->target; d->lun = f->lun; d->tag = f->tag;
        memcpy(d->cdb, f->cdb, sizeof(d->cdb)); d->cdbLen = f->cdbLen;
        d->dir = f->dir; d->length = f->len;
        d->td = f->td.data(); d->tdSize = f->td.size();
        d->timeoutMs = f->timeoutMs;
        return true;
    }
    bool TaskMapBegin(void *t, uint64_t) override {
        FakeTask *f = (FakeTask *)t;
        CHECK(!f->mapped);
        f->mapped = true;
        return true;
    }
    bool TaskMapNext(void *t, uint64_t *offset, struct mps_kext_seg *chunk, uint32_t *n) override {
        FakeTask *f = (FakeTask *)t;
        uint32_t i = 0;
        while (i < *n && *offset < f->len) {          /* 4 KiB pieces, like a DART page list */
            uint64_t l = f->len - *offset < 4096 ? f->len - *offset : 4096;
            chunk[i].addr = f->iova + *offset; chunk[i].len = l;
            *offset += l; i++;
        }
        *n = i;
        return true;
    }
    void TaskUnmap(void *t) override {
        FakeTask *f = (FakeTask *)t;
        CHECK(f->mapped);
        f->mapped = false;
    }
    void SetTimeoutForTask(void *t) override {
        FakeTask *f = (FakeTask *)t;
        CHECK(f->outstanding);
        if (ignoreTimeouts) return;
        f->armed = f->timeoutMs != 0;
        f->deadline = ioc->nowUs + (uint64_t)f->timeoutMs * 1000;
    }
    void CompleteTask(void *t, int done, uint8_t scsiStatus, uint64_t, const uint8_t *, uint32_t) override {
        FakeTask *f = (FakeTask *)t;
        CHECK(f->outstanding && f->completions == 0);
        CHECK(!f->mapped);                          /* IODMACommand completed first */
        CHECK(destroys[f->target] == f->destroysAtSubmit);   /* no completion after DestroyTargetForID */
        f->outstanding = false; f->armed = false;
        f->completions++; f->done = done; f->scsiStatus = scsiStatus;
    }
    bool CreateTarget(uint32_t id) override {
        CHECK(!nubs.count(id));
        nubs.insert(id); creates[id]++;
        if (onCreate) onCreate(id);                 /* the nub's synchronous probe: other threads run */
        return true;
    }
    void DestroyTarget(uint32_t id) override {
        nubs.erase(id); destroys[id]++;
    }
    bool CallEnter(int which) override { return calls[which].Enter(); }
    bool CallCancel(int which) override { return calls[which].Cancel(); }
    void Log(uint32_t cls, const char *line) override { mps_kext_log(nullptr, cls, line); }

    static void HostDoneKick(void *hba) {
        Family *f = (Family *)hba;
        f->glue.Lock(); f->glue.ScheduleDone(); f->glue.Unlock();
    }
    static void HostAsync(void *hba, int event, uint64_t tids, uint64_t present, const uint32_t *gens) {
        ((Family *)hba)->glue.Async(event, tids, present, gens);
    }

    /* InitializeController + StartController */
    void Attach(MockIocModel *m) {
        ioc = m;
        gHost.ioc = m;
        CHECK(glue.Init(this));
        calls[kCallHotplug].fn = [this]() { glue.HotplugCallBody(); };
        calls[kCallDone].fn = [this]() { glue.DoneCallBody(); };
        gHost.threadCalls = {&calls[kCallHotplug], &calls[kCallDone]};
        host.hba = this;
        host.io_done_kick = &Family::HostDoneKick;
        host.async = &Family::HostAsync;
        k = mps_kext_create(&host, 0x0087);
        CHECK(k != nullptr);
        glue.fMps = k;
        CHECK(mps_kext_attach(k, MPSK_DEBUG_ALL) == 0);
        gHost.sc = (struct mps_softc *)mps_kext_softc(k);
        glue.Lock(); glue.fCoreAttached = true; glue.Unlock();
        glue.fMaxSegs = mps_kext_max_segs(k);
        glue.Start();
        CHECK(glue.Enter());
        mps_kext_start(k);
        glue.Leave();
        glue.Lock(); glue.ScheduleHotplug(); glue.Unlock();
        HostRunPending();
    }
    /* StopController + TerminateController */
    void Detach() {
        glue.StopCore();
        glue.QuiesceHotplug();
        glue.QuiesceDone();
        glue.DeliverCompletions();
        for (FakeTask *t : tasks) {
            CHECK(!t->outstanding && !t->mapped);    /* every accepted task completed */
            if (t->iova) ioc->DmaFree(ioc->Ptr(t->iova, 1));
            delete t;
        }
        tasks.clear();
        mps_kext_destroy(k);
        glue.Fini();
        gHost.sc = nullptr; gHost.threadCalls.clear();
        CHECK(ioc->liveAllocs == 0);
    }

    FakeTask *NewRead(uint32_t target, uint32_t timeoutMs = 30000) {
        FakeTask *t = new FakeTask;
        tasks.push_back(t);
        t->target = target; t->tag = tasks.size(); t->timeoutMs = timeoutMs;
        t->cdb[0] = 0x28; t->cdb[5] = 1; t->cdb[8] = 8;
        t->td.resize(mps_kext_task_size());
        ioc->DmaAlloc(4096, 4096, &t->iova);
        return t;
    }
    SAS2308Glue::Response Submit(FakeTask *t) {
        t->outstanding = true;
        t->destroysAtSubmit = destroys[t->target];
        SAS2308Glue::Response r = glue.ProcessTask(t);
        if (r != SAS2308Glue::kInProcess) {
            /* the family frees a refused task: it must not stay on the timer list */
            CHECK(!t->armed);
            t->outstanding = false;
        }
        return r;
    }
    /* SCSIParallelTimer: expired tasks -> HandleTimeout (the timer entry goes first) */
    void Tick() {
        for (FakeTask *t : std::vector<FakeTask *>(tasks)) {
            if (t->armed && t->deadline <= ioc->nowUs) {
                t->armed = false;
                timeouts++;
                glue.HandleTimeout(t);
            }
        }
    }
    void Run(uint64_t ms) {
        for (uint64_t i = 0; i < ms; i += 100) { HostAdvance(100); Tick(); }
    }
    uint32_t Tid(uint16_t handle) {
        struct mps_softc *sc = gHost.sc;
        for (u_int i = 0; i < sc->sassc->maxtargets; i++)
            if (sc->sassc->targets[i].handle == handle) return i;
        CHECK(!"no target for handle");
        return 0;
    }
};

static bool Want(const char *id)
{
    const char *only = getenv("KGLUE_ONLY");
    if (only && strcmp(only, id) != 0)
        return false;
    printf("kext glue %s\n", id);
    return true;
}

static void RequestReinit(const char *why)
{
    mps_lock(gHost.sc);
    mps_xnu_request_reinit(gHost.sc, why);
    mps_unlock(gHost.sc);
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    gHost.verbose = getenv("MOCK_VERBOSE") != nullptr;

    if (Want("basic")) {
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MPS_XNU_PRI_REQ_FRAMES;
        Family f; f.Attach(&m);
        CHECK(f.nubs.size() == 2);
        FakeTask *t = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
        f.Detach();
    }

    if (Want("C1")) {
        /* a reply that never comes: the family's timer must be armed, recovery completes the task */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MPS_XNU_PRI_REQ_FRAMES;
        Family f; f.Attach(&m);
        m.holdIo = true;                            /* the firmware keeps it until a TM */
        FakeTask *t = f.NewRead(f.Tid(0x0009), 10000);
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        CHECK(t->armed);
        f.Run(15000);
        CHECK(f.timeouts == 1);
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneTimeout);
        m.holdIo = false;
        f.Detach();
    }

    if (Want("C1-backstop")) {
        /* the family timer is lost: the core's own callout (timeout + 10 s) still recovers */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MPS_XNU_PRI_REQ_FRAMES;
        Family f; f.Attach(&m);
        f.ignoreTimeouts = true;
        m.holdIo = true;
        FakeTask *t = f.NewRead(f.Tid(0x0009), 5000);
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        f.Run(25000);
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneTimeout);
        m.holdIo = false;
        f.Detach();
    }

    if (Want("L1")) {
        /* a refusal inside xpt_action (no such target) completes through CompleteParallelTask */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MPS_XNU_PRI_REQ_FRAMES;
        Family f; f.Attach(&m);
        FakeTask *t = f.NewRead(17);
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneGone);
        f.Detach();
    }

    if (Want("H1")) {
        /* the device goes while its nub is being created (CreateTargetForID probes): the destroy is not lost */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MPS_XNU_PRI_REQ_FRAMES;
        Family f;
        bool pulled = false;
        f.onCreate = [&](uint32_t) {
            if (pulled) return;
            pulled = true;
            m.PostTopologyEvent({0x0009}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
            HostRunPending();                       /* event task, rescan, a second hotplug invocation */
        };
        f.Attach(&m);
        HostRunPending();
        CHECK(pulled);
        CHECK(f.calls[SAS2308GluePort::kCallHotplug].concurrent >= 1);
        CHECK(f.nubs.empty());                      /* the nub of the removed disk is gone */
        f.Detach();
    }

    if (Want("C2")) {
        /* the same id comes back for a different disk (identity unknown): never the old nub */
        MockIocModel m; m.devices = {0x0009, 0x000B}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MPS_XNU_PRI_REQ_FRAMES;
        m.sataSerial[0x0009] = "ZR50AAAA"; m.sataSerial[0x000B] = "ZR50BBBB";
        Family f; f.Attach(&m);
        const uint32_t tb = f.Tid(0x000B);
        CHECK(f.nubs.count(tb) && f.creates[tb] == 1);
        RequestReinit("same disks");
        f.Run(3000);
        CHECK(f.creates[tb] == 1 && f.destroys[tb] == 0);   /* same disk: the nub stays */
        m.sataSerial[0x000B] = "ZR50CCCC";
        m.identifyFail.insert(0x000B);              /* and its IDENTIFY fails: identity unknown */
        RequestReinit("disk swapped");
        f.Run(30000);                               /* 5 IDENTIFY tries, 3 s spin-up wait each */
        const uint32_t tc = f.Tid(0x000B);
        CHECK(f.nubs.count(tc));
        if (tc == tb)
            CHECK(f.destroys[tb] == 1 && f.creates[tb] == 2);
        else
            CHECK(!f.nubs.count(tb));
        f.Detach();
    }

    if (Want("M7")) {
        /* a removed disk's IO completes before its nub is destroyed */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MPS_XNU_PRI_REQ_FRAMES;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        m.holdDev = 0x0009;
        FakeTask *a = f.NewRead(t9), *b = f.NewRead(t9);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess && f.Submit(b) == SAS2308Glue::kInProcess);
        m.PostTopologyEvent({0x0009}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
        f.Run(2000);
        CHECK(!f.nubs.count(t9));
        CHECK(a->completions == 1 && b->completions == 1);   /* CompleteTask checked: before the destroy */
        m.holdDev = 0;
        f.Detach();
    }

    printf("PASS (%d checks)\n", gChecks);
    return 0;
}
