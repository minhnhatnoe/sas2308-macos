/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
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
    int completions = 0, done = -1, response = -1, timeouts = 0;
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
    void SetTimeoutForTask(void *t, uint32_t ms) override {
        FakeTask *f = (FakeTask *)t;
        CHECK(f->outstanding);
        if (ignoreTimeouts) return;
        const uint32_t d = ms ? ms : f->timeoutMs;  /* 0: the task's own (SCSIParallelTimer::SetTimeout) */
        f->armed = d != 0;
        f->deadline = ioc->nowUs + (uint64_t)d * 1000;
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
        CHECK(glue.Attached());
        CHECK(glue.fSlots != nullptr);
        CHECK(glue.fCoreAttached && glue.fMaxSegs == mps_kext_max_segs(k));
        CHECK(glue.fMaxTasks == mps_kext_max_tasks(k) && glue.fMaxTasks == (uint32_t)gHost.sc->xnu.io_cmds);
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

    /* SAS2308HBA::willTerminate: our part, the family's destroy loop (IDs 0..62), ID 63 */
    void WillTerminate() {
        (void)glue.WillTerminate();
        for (uint32_t id = 0; id < SAS2308Glue::kMaxTargets - 1; id++)
            if (nubs.count(id)) DestroyTarget(id);
        glue.TerminateTargets(SAS2308Glue::kMaxTargets - 1);
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
                t->timeouts++;
                glue.HandleTimeout(t);
                /* re-armed from HandleTimeout: not expired again in the same GetExpiredTask loop (D4) */
                CHECK(!t->armed || t->deadline > ioc->nowUs);
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
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        CHECK(f.nubs.size() == 2);
        FakeTask *t = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
        f.Detach();
    }

    if (Want("tasks")) {
        /* ReportMaximumTaskCount = min(RequestCredit, 2048) - internal: that many at once, then refused */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        m.requestCredit = 10240; m.replyPostDepth = 65520;
        Family f; f.Attach(&m);
        CHECK(f.glue.fMaxTasks == MPS_REQ_FRAMES - MPS_XNU_INTERNAL_CMDS && f.glue.fMaxTasks == 2040);
        struct mps_kext_limits l;
        CHECK(mps_kext_limits(f.k, &l) == 0);
        CHECK(l.request_credit == 10240 && l.high_priority_credit == MockIocModel::kHighPriorityCredit &&
              l.max_reply_post_depth == 65520 && l.req_frames == MPS_REQ_FRAMES && l.os_commands == 2040 &&
              l.pri_req_frames == MockIocModel::kHighPriorityCredit && l.reply_frames >= l.req_frames + l.pri_req_frames &&
              l.chain_frames == MPS_CHAIN_FRAMES && l.req_frame_bytes == 128 && l.max_segs == MPS_XNU_MAXIO_PAGES + 1);
        printf("  limits: %u request + %u high-priority frames, %u replies (post queue %u), %u chains, %u OS tasks; "
               "DMA %llu B, core heap %llu B, slots %zu B, task data %zu B\n",
               l.req_frames, l.pri_req_frames, l.reply_frames, l.post_queue_depth, l.chain_frames, f.glue.fMaxTasks,
               (unsigned long long)l.dma_bytes, (unsigned long long)l.heap_bytes,
               sizeof(SAS2308Glue::Slot) * f.glue.fMaxTasks, mps_kext_task_size() * f.glue.fMaxTasks);
        m.holdIo = true;
        std::vector<FakeTask *> v;
        for (uint32_t i = 0; i < f.glue.fMaxTasks; i++) {
            v.push_back(f.NewRead(f.Tid(i % 2 ? 0x000A : 0x0009)));
            CHECK(f.Submit(v.back()) == SAS2308Glue::kInProcess);
        }
        CHECK(m.pending.size() == f.glue.fMaxTasks && f.glue.fKStats.tasksHeld == 0);
        FakeTask *extra = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(extra) == SAS2308Glue::kRejected);
        m.holdIo = false;
        m.ReleasePending();
        HostRunPending();
        for (FakeTask *t : v)
            CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
        f.Detach();
    }

    if (Want("C1")) {
        /* a reply that never comes: the family's timer must be armed, recovery completes the task */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
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
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
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
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
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
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
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
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
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
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
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

    if (Want("D5")) {
        /* Thunderbolt surprise removal with IO posted on two targets and held on a third */
        MockIocModel m; m.devices = {0x0009, 0x000A, 0x000B}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        CHECK(f.nubs.size() == 3);
        m.holdIo = true;
        FakeTask *a = f.NewRead(f.Tid(0x0009)), *b = f.NewRead(f.Tid(0x000A));
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess && f.Submit(b) == SAS2308Glue::kInProcess);
        RequestReinit("test: IO arriving now is held");
        FakeTask *c = f.NewRead(f.Tid(0x000B));
        CHECK(f.Submit(c) == SAS2308Glue::kInProcess);
        CHECK(f.glue.fKStats.tasksHeld == 1);
        m.dead = true;                              /* the dock is unplugged: registers read all-ones */
        f.WillTerminate();
        for (FakeTask *t : {a, b, c})               /* CompleteTask checks: before DestroyTargetForID */
            CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneGone);
        CHECK(f.nubs.empty());
        HostRunPending();                           /* the queued reinit finds nothing to do */
        m.holdIo = false;
        f.Detach();
    }

    if (Want("D3")) {
        /* the UNIT ATTENTION a drive returns after our resets: SYNCHRONIZE CACHE (sent once by SAM) sees GOOD */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        m.uaAfterReset = true;
        const uint32_t t9 = f.Tid(0x0009), ta = f.Tid(0x000A);
        auto sync = [&](uint32_t tid, uint64_t lun = 0) {
            FakeTask *s = f.NewRead(tid);
            s->lun = lun;
            s->cdb[0] = 0x35; s->cdb[5] = 0; s->cdb[8] = 0; s->dir = MPSK_DIR_NONE; s->len = 0;
            CHECK(f.Submit(s) == SAS2308Glue::kInProcess);
            HostRunPending();
            CHECK(s->completions == 1 && s->done == SAS2308GluePort::kDoneStatus);
            return s->scsiStatus;
        };
        /* a reinit (IOC fault, wake from sleep): every LUN */
        RequestReinit("test");
        f.Run(3000);
        CHECK(sync(t9) == 0x00 && sync(ta) == 0x00 && sync(ta, 1) == 0x00);
        CHECK(m.uaSent == 3);
        /* a target reset (recovery, or the OS): that target's LUNs */
        char why[128];
        CHECK(mps_kext_tm(f.k, MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET, t9, 0, nullptr, nullptr, 0, why,
                          sizeof(why)) == MPSK_TM_COMPLETE);
        HostRunPending();
        CHECK(sync(t9) == 0x00 && sync(t9, 2) == 0x00 && m.uaSent == 5);
        CHECK(sync(ta) == 0x00 && m.uaSent == 5);
        /* bounded: once per LUN and reset; a UA without a reset of ours reaches the OS */
        m.ua.insert({0x0009, 0});
        CHECK(sync(t9) == 0x02 && m.uaSent == 6);
        f.Detach();
    }

    if (Want("D8")) {
        /* a different disk in the bay after a wake (its first IDENTIFY failed): the old nub's IO never reaches it */
        MockIocModel m; m.devices = {0x0009, 0x000B}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        m.sataSerial[0x0009] = "ZR50AAAA"; m.sataSerial[0x000B] = "ZR50BBBB";
        Family f; f.Attach(&m);
        const uint32_t tb = f.Tid(0x000B);
        CHECK(f.creates[tb] == 1);
        m.identifyFailN[0x000B] = 5;
        m.sataSerial[0x000B] = "ZR50CCCC";
        FakeTask *held = nullptr, *late = nullptr;
        int sentBefore = 0;
        gHost.onOtherThreads = [&]() {
            struct mps_softc *sc = gHost.sc;
            if (!held && (sc->xnu.tid_unverified & (1ull << tb))) {
                sentBefore = m.ioCount[0x000B];
                held = f.NewRead(tb);               /* the old nub, identity open: held */
                CHECK(f.Submit(held) == SAS2308Glue::kInProcess);
            }
            if (!late && mps_xnu_target_gen(sc, tb) != f.glue.fTargets.Gen(tb) && f.nubs.count(tb)) {
                late = f.NewRead(tb);               /* the old nub before the hotplug thread replaced it */
                CHECK(f.Submit(late) == SAS2308Glue::kInProcess);
            }
        };
        RequestReinit("wake, disk swapped");
        f.Run(60000);
        gHost.onOtherThreads = nullptr;
        CHECK(held && held->completions == 1 && held->done == SAS2308GluePort::kDoneGone);
        if (late)
            CHECK(late->completions == 1 && late->done == SAS2308GluePort::kDoneGone);
        CHECK(m.ioCount[0x000B] == sentBefore);   /* nothing of the old disk's reached the new one */
        CHECK(f.destroys[tb] == 1 && f.creates[tb] == 2 && f.nubs.count(tb));
        /* the mechanism itself: a task from a nub of an older generation is refused */
        mps_lock(gHost.sc); gHost.sc->xnu.tid_gen[tb]++; mps_unlock(gHost.sc);
        const int sent = m.ioCount[0x000B];
        FakeTask *stale = f.NewRead(tb);
        CHECK(f.Submit(stale) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(stale->completions == 1 && stale->done == SAS2308GluePort::kDoneGone && m.ioCount[0x000B] == sent);
        mps_lock(gHost.sc); gHost.sc->xnu.tid_gen[tb]--; mps_unlock(gHost.sc);
        FakeTask *fresh = f.NewRead(tb);            /* the new nub works */
        CHECK(f.Submit(fresh) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(fresh->completions == 1 && fresh->done == SAS2308GluePort::kDoneStatus && fresh->scsiStatus == 0);
        printf("  IO from the old nub between the generation change and its destroy: %s\n", late ? "refused" : "none");
        f.Detach();
    }

    if (Want("D14")) {
        /*
         * A pulled disk's removal TM answers only after 20 s (FreeBSD arms no
         * timeout for it): its IO completes when the IOC returns it, then the
         * nub goes. The drain sleeps inside the hotplug thread call, so the
         * TM is answered from the "other threads" hook.
         */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        m.holdDev = 0x0009;                         /* the firmware keeps its IO until the TM */
        FakeTask *a = f.NewRead(t9, 0), *b = f.NewRead(t9, 0);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess && f.Submit(b) == SAS2308Glue::kInProcess);
        m.tmDeferMask = 1u << MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET;
        const uint64_t t0 = m.nowUs;
        const int resets = m.diagResetCount;
        bool answered = false;
        gHost.onOtherThreads = [&]() {
            if (answered || m.nowUs < t0 + 20000000 || m.deferredTm.empty()) return;
            CHECK(a->completions == 0 && b->completions == 0);   /* the IOC owns them: never completed by us */
            CHECK(f.nubs.count(t9));                             /* so the nub waits */
            answered = true;
            m.ReleaseTm();                          /* the reset terminates the IO */
        };
        m.PostTopologyEvent({0x0009}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
        f.Run(30000);
        gHost.onOtherThreads = nullptr;
        CHECK(answered);
        CHECK(a->completions == 1 && b->completions == 1);   /* CompleteTask checked: before the destroy */
        CHECK(!f.nubs.count(t9) && f.destroys[t9] == 1);
        CHECK(m.diagResetCount == resets);
        m.tmDeferMask = 0; m.holdDev = 0;
        f.Detach();
    }

    if (Want("D14-cap")) {
        /* the removal TM never answers: after 60 s a reinit ends the IOC's ownership, then the nub goes */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        m.holdDev = 0x0009;
        FakeTask *a = f.NewRead(t9, 0);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess);
        m.tmIgnoreMask = 1u << MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET;
        const uint64_t t0 = m.nowUs;
        const int resets = m.diagResetCount;
        gHost.onOtherThreads = [&]() {
            if (m.nowUs < t0 + 55000000)
                CHECK(a->completions == 0 && f.nubs.count(t9));
        };
        m.PostTopologyEvent({0x0009}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
        m.devices = {0x000A};                       /* pulled: the rediscovery does not find it */
        f.Run(70000);
        gHost.onOtherThreads = nullptr;
        CHECK(m.diagResetCount == resets + 1);
        CHECK(a->completions == 1 && f.destroys[t9] == 1 && !f.nubs.count(t9));
        m.tmIgnoreMask = 0; m.holdDev = 0;
        f.Detach();
    }

    if (Want("D20a")) {
        /* CHECK CONDITION whose autosense failed: completed as such, not a delivery failure to retry */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        m.autosenseFailDev = 0x0009;
        FakeTask *t = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0x02);
        m.autosenseFailDev = 0;
        f.Detach();
    }

    if (Want("D4")) {
        /*
         * IO held 25 s (the controller asleep), then posted to an IOC that
         * keeps it 10 s: the family timer counts from the post, as FreeBSD's
         * callout does (no TM). A held-then-posted task that really is
         * stuck times out once, 30 s after its post.
         */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        auto heldThenPosted = [&](FakeTask *t) {
            CHECK(mps_kext_suspend(f.k, 1000, 0) == 0);
            const uint32_t held = f.glue.fKStats.tasksHeld;
            CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            CHECK(f.glue.fKStats.tasksHeld == held + 1);
            f.Run(25000);
            CHECK(t->completions == 0);
            m.holdIo = true;                        /* the firmware keeps it */
            CHECK(mps_kext_resume(f.k) == 0);
            f.Run(3000);                            /* reinit, rediscovery: posted */
            CHECK(m.pending.size() == 1 && t->completions == 0);
        };
        FakeTask *a = f.NewRead(t9);
        heldThenPosted(a);
        size_t tms = m.tms.size();
        f.Run(10000);                               /* 38 s after the submit, 10 s after the post */
        CHECK(a->timeouts >= 1);                    /* the family's timer fired ... */
        CHECK(m.tms.size() == tms && a->completions == 0);   /* ... and was re-armed: no abort */
        m.holdIo = false;
        m.ReleasePending();
        HostRunPending();
        CHECK(a->completions == 1 && a->done == SAS2308GluePort::kDoneStatus && a->scsiStatus == 0);

        FakeTask *b = f.NewRead(t9);
        heldThenPosted(b);
        tms = m.tms.size();
        const uint64_t posted = m.nowUs;
        f.Run(26000);                               /* 29 s after the post */
        CHECK(m.tms.size() == tms && b->completions == 0);
        f.Run(4000);
        CHECK(m.tms.size() == tms + 1 && m.tms.back().type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK);
        HostRunPending();
        CHECK(b->completions == 1 && b->done == SAS2308GluePort::kDoneTimeout);
        CHECK(b->timeouts == 2);                    /* re-armed once, then timed out once */
        CHECK(m.nowUs - posted >= 30000000ull);
        m.holdIo = false;
        f.Detach();
    }

    printf("PASS (%d checks)\n", gChecks);
    return 0;
}
