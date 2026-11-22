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
#include <algorithm>
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
    int attr = MPSK_ATTR_SIMPLE;
    /* family state */
    bool outstanding = false, armed = false, mapped = false;
    uint64_t deadline = 0;
    int completions = 0, done = -1, response = -1, timeouts = 0;
    uint8_t scsiStatus = 0xFF;
    uint32_t destroysAtSubmit = 0;
};

struct SAS2308_HIDDEN Family : SAS2308GluePort {
    ~Family() { glue.FreeLock(); }                  /* SAS2308HBA::free (L7) */
    SAS2308Glue glue;
    MockIocModel *ioc = nullptr;
    struct mps_kext_host host{};
    struct mps_kext *k = nullptr;
    HostThreadCall calls[2];
    std::set<uint32_t> nubs;
    uint32_t creates[1024]{}, destroys[1024]{};
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
        d->attr = f->attr;
        return true;
    }
    int mapShortage = 0;                    /* the next n TaskMapBegin fail: no IOVA space (D47) */
    int mapCalls = 0;
    bool TaskMapBegin(void *t, uint64_t) override {
        FakeTask *f = (FakeTask *)t;
        CHECK(!f->mapped);
        mapCalls++;
        if (mapShortage > 0) { mapShortage--; return false; }
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
        if (onComplete) onComplete(f);
    }
    std::function<void(FakeTask *)> onComplete;    /* the next IO from the completion (a client that refills) */
    bool CreateTarget(uint32_t id) override {
        CHECK(!nubs.count(id));
        nubs.insert(id); creates[id]++;
        if (onCreate) onCreate(id);                 /* the nub's synchronous probe: other threads run */
        return true;
    }
    void DestroyTarget(uint32_t id) override {
        nubs.erase(id); destroys[id]++;
    }
    /* as SAS2308HBAPort: hotplug on its own taskqueue thread (L10), done on a thread call */
    struct mps_kext_tq *hotplugTq = nullptr;
    bool CallEnter(int which) override {
        return which == kCallHotplug ? (hotplugTq ? mps_kext_tq_enqueue(hotplugTq) : true) : calls[which].Enter();
    }
    bool CallCancel(int which) override {
        return which == kCallHotplug ? (hotplugTq ? mps_kext_tq_cancel(hotplugTq) : false) : calls[which].Cancel();
    }
    void Log(uint32_t cls, const char *line) override { mps_kext_log(nullptr, cls, line); }
    std::set<uint32_t> inUse;               /* disks with a mounted volume / open node (S2) */
    int inUseCalls = 0;
    bool TargetInUse(uint32_t id) override { inUseCalls++; return inUse.count(id) != 0; }
    std::set<uint32_t> ejected;             /* disks whose media macOS is ejecting / ejected (EJ) */
    int ejectCalls = 0;
    bool TargetEjected(uint32_t id) override { ejectCalls++; return ejected.count(id) != 0; }

    int hotplugBodies = 0, hotplugDepth = 0, hotplugNested = 0;
    static void HotplugBody(void *hba) {
        Family *f = (Family *)hba;
        f->hotplugBodies++;
        if (f->hotplugDepth++ > 0) f->hotplugNested++;
        f->glue.HotplugCallBody();
        f->hotplugDepth--;
    }

    bool loseKicks = false;                 /* a done kick that never arrives (D43) */
    static void HostDoneKick(void *hba) {
        Family *f = (Family *)hba;
        if (f->loseKicks) return;
        f->glue.Lock(); f->glue.ScheduleDone(); f->glue.Unlock();
    }
    int asyncCalls = 0;
    static void HostAsync(void *hba, int event, uint32_t tid, const uint8_t *present, const uint32_t *gens,
                          uint32_t count) {
        ((Family *)hba)->asyncCalls++;
        ((Family *)hba)->glue.Async(event, tid, present, gens, count);
    }

    /* InitializeController + StartController */
    void Attach(MockIocModel *m) {
        ioc = m;
        gHost.ioc = m;
        CHECK(glue.Init(this));
        hotplugTq = mps_kext_tq_create("sas2308 hotplug", &Family::HotplugBody, this);   /* L10 */
        CHECK(hotplugTq != nullptr);
        calls[kCallDone].fn = [this]() { glue.DoneCallBody(); };
        gHost.threadCalls = {&calls[kCallDone]};
        host.hba = this;
        host.io_done_kick = &Family::HostDoneKick;
        host.async = &Family::HostAsync;
        k = mps_kext_create(&host, 0x0087);
        CHECK(k != nullptr);
        glue.fMps = k;
        CHECK(mps_kext_attach(k, MPSK_DEBUG_ALL) == 0);
        gHost.sc = (struct mps_softc *)mps_kext_softc(k);
        CHECK(glue.Attached());
        CHECK(glue.fCoreAttached && glue.fMaxSegs == mps_kext_max_segs(k));
        /* D21/D58: one frame fewer than the SIM's openings (FreeBSD's reqs = num_reqs - num_prireqs - 1) */
        CHECK(glue.fMaxTasks == mps_kext_max_tasks(k) &&
              glue.fMaxTasks == (uint32_t)gHost.sc->sassc->sim->max_tagged_dev_openings - 1 &&
              glue.fMaxTasks == (uint32_t)(gHost.sc->num_reqs - gHost.sc->num_prireqs - 1 - 1));
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
        mps_kext_tq_free(hotplugTq);                /* ReleaseResources */
        hotplugTq = nullptr;
        glue.QuiesceDone();
        glue.DeliverCompletions();
        for (FakeTask *t : tasks) {
            CHECK(!t->outstanding && !t->mapped);    /* every accepted task completed */
            if (t->iova) ioc->DmaFree(ioc->Ptr(t->iova, 1));
            delete t;
        }
        tasks.clear();
        CHECK(glue.TakeCore() == k);                /* ReleaseResources (L7) */
        mps_kext_destroy(k);
        glue.Fini();
        gHost.sc = nullptr; gHost.threadCalls.clear();
        CHECK(ioc->liveAllocs == 0);
    }

    /* SAS2308HBA::willTerminate: our part, the family's destroy loop (IDs 0..highest-1), the highest */
    void WillTerminate() {
        (void)glue.WillTerminate();
        const uint32_t highest = glue.fMaxTargets - 1;      /* ReportHighestSupportedDeviceID */
        for (uint32_t id = 0; id < highest; id++)
            if (nubs.count(id)) DestroyTarget(id);
        glue.TerminateTargets(highest);
    }
    FakeTask *NewRead(uint32_t target, uint32_t timeoutMs = 30000) {
        FakeTask *t = new FakeTask;
        tasks.push_back(t);
        t->target = target; t->tag = tasks.size(); t->timeoutMs = timeoutMs;
        t->cdb[0] = 0x28; t->cdb[5] = 1; t->cdb[8] = 8;
        t->td.resize(mps_kext_task_size(k));
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
        CHECK(f.glue.fMaxTasks == MPS_REQ_FRAMES - 1 - 1 && f.glue.fMaxTasks == 2046);   /* SMID 0 + 1 (D58, D21) */
        struct mps_kext_limits l;
        CHECK(mps_kext_limits(f.k, &l) == 0);
        CHECK(l.request_credit == 10240 && l.high_priority_credit == MockIocModel::kHighPriorityCredit &&
              l.max_reply_post_depth == 65520 && l.req_frames == MPS_REQ_FRAMES && l.os_commands == 2046 &&
              l.pri_req_frames == MockIocModel::kHighPriorityCredit && l.reply_frames >= l.req_frames + l.pri_req_frames &&
              l.chain_frames == MPS_CHAIN_FRAMES && l.req_frame_bytes == 128 && l.max_segs == kMaxIoPages + 1);
        printf("  limits: %u request + %u high-priority frames, %u replies (post queue %u), %u chains, %u OS tasks; "
               "DMA %llu B, core heap %llu B, task data %zu B\n",
               l.req_frames, l.pri_req_frames, l.reply_frames, l.post_queue_depth, l.chain_frames, f.glue.fMaxTasks,
               (unsigned long long)l.dma_bytes, (unsigned long long)l.heap_bytes,
               mps_kext_task_size(f.k) * f.glue.fMaxTasks);
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
        /*
         * A reply that never comes: something must time the task (review
         * C1). The core's callout, armed at the post at the task's own
         * timeout, as FreeBSD mps; no family timer per IO (batch G2).
         */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        CHECK(mps_kext_knob(f.k, MPSK_KNOB_FAMILY_TIMER) == 0);
        m.holdIo = true;                            /* the firmware keeps it until a TM */
        FakeTask *t = f.NewRead(f.Tid(0x0009), 10000);
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        CHECK(!t->armed);                           /* no SetTimeoutForTask: no gate trip, no Rearm */
        f.Run(9900);
        CHECK(m.tms.empty() && t->completions == 0);
        f.Run(5100);                                /* 10 s after the post: abort, then the task */
        CHECK(f.timeouts == 0 && !m.tms.empty() && m.tms[0].type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK);
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneTimeout);
        CHECK(gHost.sc->xnu.stats.io_timeouts == 1 && f.glue.fKStats.familyTimeouts == 0);
        m.holdIo = false;
        f.Detach();
    }

    if (Want("C1-family")) {
        /* sas2308_family_timer 1 (the scheme before G2): the family's timer fires first */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_FAMILY_TIMER, 1) == 0);
        m.holdIo = true;
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
        /* sas2308_family_timer 1 and the family timer is lost: the core's callout (timeout + 10 s) still recovers */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_FAMILY_TIMER, 1) == 0);
        f.ignoreTimeouts = true;
        m.holdIo = true;
        FakeTask *t = f.NewRead(f.Tid(0x0009), 5000);
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        f.Run(14000);
        CHECK(t->completions == 0);                 /* not at 5 s: the backstop is 10 s later */
        f.Run(11000);
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
        /* L10: the hotplug work has one thread (a taskqueue): the removal queues a second run, none nests */
        CHECK(f.hotplugNested == 0 && f.hotplugBodies >= 2);
        CHECK(f.nubs.empty());                      /* the nub of the removed disk is gone */
        f.Detach();
    }

    if (Want("L10")) {
        /* the hotplug taskqueue: a request while queued coalesces, a cancel removes it, after the
         * quiesce nothing runs and freeing the queue runs nothing either */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const int bodies = f.hotplugBodies;
        CHECK(!mps_kext_tq_enqueue(f.hotplugTq) && mps_kext_tq_enqueue(f.hotplugTq));   /* new, then coalesced */
        CHECK(mps_kext_tq_cancel(f.hotplugTq) && !mps_kext_tq_cancel(f.hotplugTq));
        HostRunPending();
        CHECK(f.hotplugBodies == bodies);
        f.glue.Lock(); f.glue.ScheduleHotplug(); f.glue.Unlock();
        HostRunPending();
        CHECK(f.hotplugBodies == bodies + 1);
        f.glue.QuiesceHotplug();
        f.glue.Lock(); f.glue.ScheduleHotplug(); f.glue.Unlock();   /* stopped: not queued */
        HostRunPending();
        CHECK(f.hotplugBodies == bodies + 1);
        f.Detach();
        CHECK(f.hotplugBodies == bodies + 1);
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
                          sizeof(why), 0) == MPSK_TM_COMPLETE);
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
            if (!held && sc->xnu.tids[tb].unverified) {
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
        mps_lock(gHost.sc); gHost.sc->xnu.tids[tb].gen++; mps_unlock(gHost.sc);
        const int sent = m.ioCount[0x000B];
        FakeTask *stale = f.NewRead(tb);
        CHECK(f.Submit(stale) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(stale->completions == 1 && stale->done == SAS2308GluePort::kDoneGone && m.ioCount[0x000B] == sent);
        mps_lock(gHost.sc); gHost.sc->xnu.tids[tb].gen--; mps_unlock(gHost.sc);
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
         * keeps it 10 s: timed from the post, as FreeBSD's callout (no TM).
         * A held-then-posted task that really is stuck times out once, 30 s
         * after its post. With the core callout as the IO timer (G2) this is
         * the callout's own semantics; with sas2308_family_timer 1 the
         * family's timer is re-armed while the task is held.
         */
        for (int familyTimer : {0, 1}) {
            MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
            m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
            Family f; f.Attach(&m);
            CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_FAMILY_TIMER, (uint32_t)familyTimer) == 0);
            const uint32_t t9 = f.Tid(0x0009);
            auto heldThenPosted = [&](FakeTask *t) {
                CHECK(mps_kext_suspend(f.k, 1000, 0) == 0);
                const uint32_t held = f.glue.fKStats.tasksHeld;
                CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
                CHECK(f.glue.fKStats.tasksHeld == held + 1);
                f.Run(25000);
                CHECK(t->completions == 0);
                m.holdIo = true;                    /* the firmware keeps it */
                CHECK(mps_kext_resume(f.k) == 0);
                f.Run(3000);                        /* reinit, rediscovery: posted */
                CHECK(m.pending.size() == 1 && t->completions == 0);
            };
            FakeTask *a = f.NewRead(t9);
            heldThenPosted(a);
            size_t tms = m.tms.size();
            f.Run(10000);                           /* 38 s after the submit, 10 s after the post */
            CHECK(familyTimer ? a->timeouts >= 1 : a->timeouts == 0);   /* the family's timer fired, re-armed */
            CHECK(m.tms.size() == tms && a->completions == 0);   /* no abort */
            m.holdIo = false;
            m.ReleasePending();
            HostRunPending();
            CHECK(a->completions == 1 && a->done == SAS2308GluePort::kDoneStatus && a->scsiStatus == 0);

            FakeTask *b = f.NewRead(t9);
            heldThenPosted(b);
            tms = m.tms.size();
            const uint64_t posted = m.nowUs;
            f.Run(26000);                           /* 29 s after the post */
            CHECK(m.tms.size() == tms && b->completions == 0);
            f.Run(4000);
            CHECK(m.tms.size() == tms + 1 && m.tms.back().type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK);
            HostRunPending();
            CHECK(b->completions == 1 && b->done == SAS2308GluePort::kDoneTimeout);
            CHECK(b->timeouts == (familyTimer ? 2 : 0));     /* family: re-armed once, then timed out once */
            CHECK(m.nowUs - posted >= 30000000ull);
            m.holdIo = false;
            f.Detach();
        }
    }

    if (Want("D15")) {
        /*
         * SAM sends START STOP UNIT with timeout 0 (no family timer). Its
         * reply is lost: the core's callout recovers it after 60 s (da's
         * DA_DEFAULT_TIMEOUT; a spin-up measured 10.5-11.7 s, D15b), so the
         * PM thread does not hang. Another untimed command (a pass-through)
         * gets 10 min; FORMAT UNIT keeps no timer.
         */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        auto nodata = [&](uint32_t tid, uint8_t op) {
            FakeTask *t = f.NewRead(tid, 0);
            memset(t->cdb, 0, sizeof(t->cdb));
            t->cdb[0] = op; t->cdbLen = 6; t->dir = MPSK_DIR_NONE; t->len = 0;
            CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            CHECK(!t->armed);                       /* the family contract: 0 = no timer */
            return t;
        };
        m.holdIo = true;
        FakeTask *ssu = nodata(f.Tid(0x0009), 0x1B);
        FakeTask *tur = nodata(f.Tid(0x000A), 0x00);
        f.Run(55000);
        CHECK(ssu->completions == 0);
        f.Run(15000);                               /* 70 s */
        CHECK(ssu->completions == 1 && ssu->done == SAS2308GluePort::kDoneTimeout);
        CHECK(tur->completions == 0);
        f.Run(520000);                              /* 590 s */
        CHECK(tur->completions == 0);
        FakeTask *fmt = nodata(f.Tid(0x0009), 0x04);
        f.Run(25000);                               /* 615 s */
        CHECK(tur->completions == 1 && tur->done == SAS2308GluePort::kDoneTimeout);
        f.Run(600000);
        CHECK(fmt->completions == 0);               /* runs for hours on an 18 TB disk */
        m.holdIo = false;
        m.ReleasePending();
        HostRunPending();
        CHECK(fmt->completions == 1 && fmt->done == SAS2308GluePort::kDoneStatus);
        f.Detach();
    }

    /*
     * Batch F: the 30 s halt watchdog (PEHaltRestartInternal) covers the
     * whole halt, so the 10 min backstop of an untimed task (D15) and the
     * 30 s + 10 s of a read cannot save a lost reply there. Once the halt
     * begins (priority interest, before any systemWillShutdown) every
     * command has at most 10 s left (FreeBSD's SSU timeout), a TM at most
     * 3 s, and a reinit is the shutdown stop (one MUR, no diag reset).
     */
    auto haltRig = [](MockIocModel &m) {
        m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
    };
    auto noData = [](Family &f, uint32_t tid, uint8_t op) {
        FakeTask *t = f.NewRead(tid, 0);
        memset(t->cdb, 0, sizeof(t->cdb));
        t->cdb[0] = op; t->cdbLen = 6; t->dir = MPSK_DIR_NONE; t->len = 0;
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        return t;
    };

    if (Want("F")) {
        /* an SSU (timeout 0) and a read posted before the halt lose their replies */
        MockIocModel m; haltRig(m);
        Family f; f.Attach(&m);
        m.dropCompletion = true;
        FakeTask *ssu = noData(f, f.Tid(0x0009), 0x1B);
        FakeTask *rd = f.NewRead(f.Tid(0x000A));
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        f.Run(5000);
        const int diags = m.diagResetCount;
        mps_kext_halt_begin(f.k);
        f.Run(14000);
        CHECK(ssu->completions == 0 && rd->completions == 0);     /* capped, not failed */
        f.Run(6000);                                /* 20 s after the halt began */
        HostRunPending();
        CHECK(ssu->completions == 1 && ssu->done != SAS2308GluePort::kDoneStatus);
        CHECK(rd->completions == 1 && rd->done != SAS2308GluePort::kDoneStatus);
        CHECK(m.diagResetCount == diags);           /* the stop, not a reinit */
        CHECK(gHost.sc->xnu.stats.halt_stops == 1 && gHost.sc->xnu.stats.halt_capped == 2);
        FakeTask *late = f.NewRead(f.Tid(0x0009));  /* the IOC is stopped: fails at once */
        f.Submit(late);
        HostRunPending();
        CHECK(late->completions == 1 && late->done == SAS2308GluePort::kDoneGone);
        m.dropCompletion = false;
        f.Detach();
    }

    if (Want("F-new")) {
        /* IO after the halt began gets 15 s; an abort that works keeps the controller */
        MockIocModel m; haltRig(m);
        Family f; f.Attach(&m);
        mps_kext_halt_begin(f.k);
        m.holdIo = true;
        FakeTask *rd = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        FakeTask *fmt = noData(f, f.Tid(0x000A), 0x04);   /* FORMAT UNIT: untimed (D15), capped too */
        f.Run(14500);
        CHECK(rd->completions == 0 && fmt->completions == 0);
        f.Run(1500);
        HostRunPending();
        CHECK(rd->completions == 1 && rd->done != SAS2308GluePort::kDoneStatus);
        CHECK(fmt->completions == 1 && fmt->done != SAS2308GluePort::kDoneStatus);
        CHECK(gHost.sc->xnu.stats.halt_stops == 0);
        m.holdIo = false;
        FakeTask *ok = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(ok) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(ok->completions == 1 && ok->done == SAS2308GluePort::kDoneStatus);
        f.Detach();
    }

    if (Want("F-spinup")) {
        /*
         * D15b: a command at the halt that waits for a spin-up from standby
         * (10.5-11.7 s measured) completes normally: no abort, no TM.
         */
        MockIocModel m; haltRig(m);
        Family f; f.Attach(&m);
        mps_kext_halt_begin(f.k);
        m.holdIo = true;
        FakeTask *ssu = noData(f, f.Tid(0x0009), 0x1B);
        FakeTask *rd = f.NewRead(f.Tid(0x000A));
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        f.Run(12000);                               /* the drives spin up */
        CHECK(m.tms.empty() && ssu->completions == 0 && rd->completions == 0);
        m.holdIo = false;
        m.ReleasePending();
        HostRunPending();
        CHECK(ssu->completions == 1 && ssu->done == SAS2308GluePort::kDoneStatus);
        CHECK(rd->completions == 1 && rd->done == SAS2308GluePort::kDoneStatus);
        CHECK(m.tms.empty() && gHost.sc->xnu.stats.halt_stops == 0 && gHost.sc->xnu.stats.io_timeouts == 0);
        f.Detach();
    }

    if (Want("F-tm")) {
        /* the IOC answers no TM: the abort's timeout (3 s) leads to the stop */
        MockIocModel m; haltRig(m);
        Family f; f.Attach(&m);
        m.dropCompletion = true; m.tmIgnoreMask = ~0u;
        FakeTask *rd = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        f.Run(2000);
        const int diags = m.diagResetCount;
        mps_kext_halt_begin(f.k);
        f.Run(21000);                               /* 15 s + 3 s + the stop (<= 2 s) */
        HostRunPending();
        CHECK(rd->completions == 1 && rd->done != SAS2308GluePort::kDoneStatus);
        CHECK(m.diagResetCount == diags && gHost.sc->xnu.stats.halt_stops == 1);
        m.dropCompletion = false; m.tmIgnoreMask = 0;
        f.Detach();
    }

    if (Want("F-pending-tm")) {
        /* a LUN reset already waiting (30 s) when the halt begins: 3 s left */
        MockIocModel m; haltRig(m);
        Family f; f.Attach(&m);
        m.dropCompletion = true;
        m.tmNoTermMask = 1u << MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK;
        m.tmIgnoreMask = 1u << MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET;
        FakeTask *rd = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        f.Run(31000);                               /* family timer, abort, LUN reset sent */
        CHECK(!m.tms.empty() && m.tms.back().type == MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET);
        CHECK(rd->completions == 0);
        mps_kext_halt_begin(f.k);
        f.Run(6000);
        HostRunPending();
        CHECK(rd->completions == 1 && gHost.sc->xnu.stats.halt_stops == 1);
        m.dropCompletion = false; m.tmNoTermMask = m.tmIgnoreMask = 0;
        f.Detach();
    }

    if (Want("F-reinit")) {
        /* a reinit whose diag reset never ends is running when the halt begins */
        MockIocModel m; haltRig(m);
        Family f; f.Attach(&m);
        m.holdIo = true;
        FakeTask *rd = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        m.diagStuck = true;
        bool fired = false;
        uint64_t t0 = 0;
        gHost.onOtherThreads = [&]() {
            struct mps_softc *sc = gHost.sc;
            if (!fired && sc->xnu.reinit_running && (sc->mps_flags & MPS_FLAGS_DIAGRESET)) {
                fired = true; t0 = m.nowUs;
                mps_kext_halt_begin(f.k);
            }
        };
        RequestReinit("IOC fault (test)");
        for (int i = 0; i < 600 && !(fired && rd->completions && !gHost.sc->xnu.reinit_running); i++)
            f.Run(100);
        gHost.onOtherThreads = nullptr;
        CHECK(fired && !gHost.sc->xnu.reinit_running);
        CHECK(rd->completions == 1 && rd->done != SAS2308GluePort::kDoneStatus);
        CHECK(m.nowUs - t0 <= 5000000ull && gHost.sc->xnu.stats.halt_stops == 1);
        m.diagStuck = false; m.holdIo = false;
        f.Detach();
    }

    if (Want("D10")) {
        /*
         * OS task management goes through FreeBSD's paths: an abort of a
         * posted task joins the target's timeout recovery (one TM per
         * target at a time), a target or LUN reset is XPT_RESET_DEV (held
         * behind a recovery like CAM's device queue), an abort of a held
         * task completes it, ABORT TASK SET / CLEAR TASK SET / CLEAR ACA
         * are rejected.
         */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009), ta = f.Tid(0x000A);
        char why[192];
        m.holdDev = 0x0009;
        m.tmDeferMask = (1u << MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK) |
                        (1u << MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET) |
                        (1u << MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET);
        FakeTask *a = f.NewRead(t9), *b = f.NewRead(t9);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess);
        f.Run(5000);
        CHECK(f.Submit(b) == SAS2308Glue::kInProcess);
        f.Run(26000);                               /* a's timer: the recovery's ABORT_TASK, not answered yet */
        CHECK(gHost.sc->xnu.stats.io_timeouts == 1 && m.deferredTm.size() == 1);
        const size_t tms0 = m.tms.size();
        size_t maxDeferred = 0;
        uint64_t lastRelease = m.nowUs;
        gHost.onOtherThreads = [&]() {              /* the IOC answers each TM 1 s after it arrived */
            maxDeferred = std::max(maxDeferred, m.deferredTm.size());
            if (!m.deferredTm.empty() && m.nowUs - lastRelease >= 1000000) {
                lastRelease = m.nowUs;
                m.ReleaseTm();
            }
        };
        CHECK(mps_kext_tm(f.k, MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK, t9, 0, b->td.data(), b, b->tag, why,
                          sizeof(why), 0) == MPSK_TM_COMPLETE);
        HostRunPending();
        CHECK(maxDeferred == 1);                    /* one TM at a time on the target */
        CHECK(a->completions == 1 && b->completions == 1);
        for (size_t i = tms0 - 1; i < m.tms.size(); i++)
            CHECK(m.tms[i].type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK && m.tms[i].dev == 0x0009);
        /* a target reset while a recovery runs on that target waits for it */
        FakeTask *c = f.NewRead(t9);
        CHECK(f.Submit(c) == SAS2308Glue::kInProcess);
        f.Run(31000);                               /* c's recovery starts */
        maxDeferred = 0;
        const size_t tms1 = m.tms.size();
        CHECK(mps_kext_tm(f.k, MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET, t9, 0, nullptr, nullptr, 0, why,
                          sizeof(why), 0) == MPSK_TM_COMPLETE);
        CHECK(maxDeferred == 1 && m.tms.size() == tms1 + 1);
        CHECK(m.tms.back().type == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET);
        CHECK(m.tms.back().msgFlags == MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET);
        /* a LUN reset is FreeBSD's XPT_RESET_DEV too: a target reset */
        CHECK(mps_kext_tm(f.k, MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET, ta, 0, nullptr, nullptr, 0, why,
                          sizeof(why), 0) == MPSK_TM_COMPLETE);
        CHECK(m.tms.back().type == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET && m.tms.back().dev == 0x000A);
        gHost.onOtherThreads = nullptr;
        HostRunPending();
        CHECK(c->completions == 1);
        /* the rest: rejected, no TM */
        const size_t tms2 = m.tms.size();
        for (uint8_t type : {MPI2_SCSITASKMGMT_TASKTYPE_ABRT_TASK_SET, MPI2_SCSITASKMGMT_TASKTYPE_CLEAR_TASK_SET,
                             MPI2_SCSITASKMGMT_TASKTYPE_CLR_ACA})
            CHECK(mps_kext_tm(f.k, type, ta, 0, nullptr, nullptr, 0, why, sizeof(why), 0) == MPSK_TM_REJECTED);
        CHECK(m.tms.size() == tms2);
        /* an abort of a held task completes it without the IOC */
        m.holdDev = 0; m.tmDeferMask = 0;
        CHECK(mps_kext_suspend(f.k, 1000, 0) == 0);
        FakeTask *h = f.NewRead(ta);
        CHECK(f.Submit(h) == SAS2308Glue::kInProcess);
        CHECK(mps_kext_tm(f.k, MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK, ta, 0, h->td.data(), h, h->tag, why,
                          sizeof(why), 0) == MPSK_TM_COMPLETE);
        HostRunPending();
        CHECK(h->completions == 1 && h->done == SAS2308GluePort::kDoneRetry && m.tms.size() == tms2);
        CHECK(mps_kext_resume(f.k) == 0);
        f.Run(3000);
        f.Detach();
    }

    if (Want("L7")) {
        /* the glue lock lives until free() (FreeBSD: entry points drained, then mtx_destroy) */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        FakeTask *a = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess);
        HostRunPending();
        struct mps_kext_snapshot s;
        CHECK(f.glue.Snapshot(&s) && s.st.ios == 1 && !s.gone);   /* attached: through the gate */
        f.glue.StopCore();                          /* mps_free */
        CHECK(f.glue.Snapshot(&s) && s.st.ios == 1 && s.gone);    /* detached: the final counters */
        f.glue.SetDebug(MPS_INFO | MPS_FAULT);
        f.Detach();                                 /* TerminateController: TakeCore, destroy, Fini */
        CHECK(f.glue.HasLock() && !f.glue.Enter());
        CHECK(!f.glue.Snapshot(&s));                /* setProperties after the stop: nothing, no UAF */
        f.glue.SetDebug(MPS_INFO);
        f.glue.FreeLock();                          /* free() */
        CHECK(!f.glue.HasLock() && !f.glue.Snapshot(&s));
    }

    if (Want("L17")) {
        /* the INTx fallback runs FreeBSD's mps_intr: a line raised with HIS REPLY_DESCRIPTOR_INTERRUPT
         * clear (another device on a shared pin) processes nothing; with it set, the replies */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        FakeTask *a = f.NewRead(f.Tid(0x0009));
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess);
        const uint32_t post = m.hostPost;
        CHECK(m.iocPost != post);                   /* the reply is posted */
        m.hisRdiOff = true;
        mps_kext_intr_intx(f.k);
        CHECK(m.hostPost == post && a->completions == 0);
        m.hisRdiOff = false;
        CHECK(f.glue.Enter());                      /* HandleInterruptRequest */
        mps_kext_intr_intx(f.k);
        f.glue.Leave();
        f.glue.DeliverCompletions();
        CHECK(m.hostPost != post && a->completions == 1);
        f.Detach();
    }

    if (Want("D43")) {
        /*
         * FreeBSD drains replies only from the interrupt, and polls only when
         * the waiter cannot rely on it (mps_wait_command: NO_SLEEP). The TM
         * waiter pumps only when its caller holds the work-loop gate (the
         * interrupt's action could not run); otherwise it sleeps and the
         * interrupt completes the TM. The 1 s timer delivers nothing: a
         * completion left with no kick for two ticks is counted and logged
         * once, and goes out with the next delivery (interrupt, family timeout).
         */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        char why[128];
        /* in the gate, interrupts not delivered: the waiter's pump completes it */
        m.him |= MPI2_HIM_RIM;
        CHECK(mps_kext_tm(f.k, MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET, t9, 0, nullptr, nullptr, 0, why,
                          sizeof(why), 1) == MPSK_TM_COMPLETE);
        /* not in the gate: no pump; the TM completes once the interrupt is delivered again */
        const uint64_t t0 = m.nowUs;
        uint64_t unmaskAt = 0;
        bool early = false;
        const uint32_t post0 = m.hostPost;
        gHost.onOtherThreads = [&] {
            if (unmaskAt == 0 && m.nowUs >= t0 + 200000) {
                early = m.hostPost != post0;       /* nothing consumed while masked */
                unmaskAt = m.nowUs;
                m.him &= ~MPI2_HIM_RIM;
            }
        };
        CHECK(mps_kext_tm(f.k, MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET, t9, 0, nullptr, nullptr, 0, why,
                          sizeof(why), 0) == MPSK_TM_COMPLETE);
        gHost.onOtherThreads = nullptr;
        CHECK(unmaskAt != 0 && !early && m.hostPost != post0);
        HostRunPending();
        /* the 1 s timer: detects, counts and logs, kicks the done call (it does not deliver itself) */
        const size_t mark = gHost.log.size();
        f.glue.TimerDeliver();                      /* nothing waiting: nothing counted */
        CHECK(f.glue.fKStats.lostKicks == 0);
        f.loseKicks = true;
        FakeTask *a = f.NewRead(t9), *b = f.NewRead(t9);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess && f.Submit(b) == SAS2308Glue::kInProcess);
        mps_intr_msi(gHost.sc);                     /* completed off the interrupt path: kicks the done call */
        HostRunPending();
        CHECK(a->completions == 0 && b->completions == 0);   /* the kick was lost */
        f.glue.TimerDeliver();                      /* first tick: maybe a kick on its way */
        CHECK(f.glue.fKStats.lostKicks == 0 && a->completions == 0);
        f.glue.TimerDeliver();                      /* second tick: lost; the timer kicks the done call */
        f.glue.TimerDeliver();
        CHECK(f.glue.fKStats.lostKicks == 1 && a->completions == 0 && b->completions == 0);
        HostRunPending();                           /* the done call delivers (no family timer per IO since G2) */
        CHECK(a->completions == 1 && b->completions == 1);
        f.glue.TimerDeliver();
        f.loseKicks = false;
        FakeTask *c = f.NewRead(t9);
        CHECK(f.Submit(c) == SAS2308Glue::kInProcess);
        mps_intr_msi(gHost.sc);                     /* kicked: the done call is queued, not run yet */
        f.glue.TimerDeliver();
        f.glue.TimerDeliver();
        CHECK(c->completions == 0 && f.glue.fKStats.lostKicks == 1);
        HostRunPending();
        CHECK(c->completions == 1);
        size_t lines = 0;
        for (size_t i = mark; i < gHost.log.size(); i++)
            lines += gHost.log[i].line.find("no completion kick") != std::string::npos;
        CHECK(lines == 1);
        f.Detach();
    }

    if (Want("D36")) {
        /* nothing posts AC_LOST_DEVICE (only CAM's probe does): the kext does not listen for it */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        struct mps_softc *sc = gHost.sc;
        struct cam_path *path;
        mps_lock(sc);
        CHECK(xpt_create_path(&path, NULL, cam_sim_path(sc->sassc->sim), 0, 0) == CAM_REQ_CMP);
        const int calls = f.asyncCalls;
        xpt_async(AC_LOST_DEVICE, path, NULL);
        CHECK(f.asyncCalls == calls);
        xpt_free_path(path);
        mps_unlock(sc);
        f.Detach();
    }

    if (Want("D7")) {
        /* 70 devices, MaxTargets 128: 70 nubs (the 65th had none); the highest ID destroyed at termination */
        MockIocModel m; m.maxTargets = 128; m.strictEventMasks = false;
        m.devices.clear();
        for (uint16_t i = 0; i < 70; i++)
            m.devices.push_back((uint16_t)(0x0100 + i));
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        CHECK(f.glue.fMaxTargets == mps_kext_max_targets(f.k) && f.glue.fMaxTargets >= 128);
        CHECK(f.nubs.size() == 70);
        const uint32_t last = f.Tid(0x0100 + 69);
        CHECK(f.nubs.count(last) && last >= 64);
        FakeTask *t = f.NewRead(last);
        CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
        /* the 70th removed: its nub goes */
        m.devices.pop_back();
        m.PostTopologyEvent({(uint16_t)(0x0100 + 69)}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
        HostRunPending();
        CHECK(f.nubs.size() == 69 && !f.nubs.count(last) && f.destroys[last] == 1);
        f.WillTerminate();
        CHECK(f.nubs.empty());
        f.Detach();
    }

    if (Want("D9")) {
        /* FreeBSD's shutdown: START STOP UNIT (stop) to SATA SSDs only (enable_ssu default), before the MUR */
        MockIocModel m; m.devices = {0x0009, 0x000B, 0x000C}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        m.sataSerial[0x000B] = "S6SSD001"; m.sataSsd.insert(0x000B);      /* SATA SSD */
        m.sataSerial[0x000C] = "ZR50HDD0";                                /* SATA HDD */
        Family f; f.Attach(&m);
        for (uint16_t h : {0x0009, 0x000B, 0x000C}) {                     /* SAM's INQUIRY at nub creation */
            FakeTask *q = f.NewRead(f.Tid(h));
            q->cdb[0] = 0x12; q->cdb[1] = 0; q->cdb[4] = 36; q->cdb[5] = 0; q->cdbLen = 6; q->len = 36;
            CHECK(f.Submit(q) == SAS2308Glue::kInProcess);
            HostRunPending();
            CHECK(q->completions == 1 && q->done == SAS2308GluePort::kDoneStatus);
        }
        auto ssus = [&m]() {
            std::vector<MockIocModel::ScsiOp> v;
            for (const auto &o : m.scsiOps) if (o.op == 0x1B) v.push_back(o);
            return v;
        };
        CHECK(ssus().empty());
        const int murs = m.murCount;
        CHECK(mps_kext_suspend(f.k, 1000, 1) == 0);                       /* systemWillShutdown */
        std::vector<MockIocModel::ScsiOp> v = ssus();
        CHECK(v.size() == 1 && v[0].handle == 0x000B && v[0].b4 == 0 && v[0].b1 == 0);   /* stop, not immediate */
        CHECK(m.murCount == murs + 1);                                     /* then the IOC stopped */
        f.Detach();
    }

    if (Want("D45")) {
        /* no slot table: the glue's per-task state is in the task's HBA data, the family finds tasks */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        m.holdIo = true;
        FakeTask *a = f.NewRead(f.Tid(0x0009)), *z = f.NewRead(f.Tid(0x0009));
        z->dir = MPSK_DIR_NONE; z->len = 0; z->cdb[0] = 0x00;
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess && f.Submit(z) == SAS2308Glue::kInProcess);
        CHECK(mps_kext_task_glue(a->td.data())->prepared && !mps_kext_task_glue(z->td.data())->prepared);
        CHECK(f.glue.fOutstanding == 2);
        m.holdIo = false;
        m.ReleasePending();
        HostRunPending();
        CHECK(a->completions == 1 && z->completions == 1 && !a->mapped);
        CHECK(!mps_kext_task_glue(a->td.data())->prepared && f.glue.fOutstanding == 0);
        f.Detach();
    }

    if (Want("D47")) {
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        /* a data direction with length 0 is a command without data (FreeBSD: dxfer_len 0, cm_data NULL) */
        for (int dir : {MPSK_DIR_IN, MPSK_DIR_OUT}) {
            FakeTask *z = f.NewRead(t9);
            z->dir = dir; z->len = 0; z->cdb[0] = 0x35; z->cdbLen = 10;     /* SYNCHRONIZE CACHE */
            CHECK(f.Submit(z) == SAS2308Glue::kInProcess);
            CHECK(m.lastDataLength == 0 &&
                  (m.lastControl & (MPI2_SCSIIO_CONTROL_READ | MPI2_SCSIIO_CONTROL_WRITE)) == 0);
            HostRunPending();
            CHECK(z->completions == 1 && z->done == SAS2308GluePort::kDoneStatus && z->scsiStatus == 0);
        }
        /* no IOVA space: the task waits (in order) and is mapped again later, not failed */
        f.mapShortage = 3;
        FakeTask *a = f.NewRead(t9), *b = f.NewRead(t9);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess && a->armed);
        CHECK(f.Submit(b) == SAS2308Glue::kInProcess && b->armed);  /* behind a: not mapped before it */
        HostRunPending();                                           /* the done call retries: short again */
        CHECK(a->completions == 0 && b->completions == 0 && !a->mapped && !b->mapped);
        CHECK(f.glue.fKStats.mapWaits == 2);
        for (int i = 0; i < 3 && a->completions == 0; i++) {
            f.glue.TimerDeliver();                                  /* the 1 s timer retries */
            HostRunPending();
        }
        CHECK(a->completions == 1 && a->done == SAS2308GluePort::kDoneStatus && a->scsiStatus == 0);
        CHECK(b->completions == 1 && b->done == SAS2308GluePort::kDoneStatus);
        CHECK(f.mapShortage == 0 && f.glue.fOutstanding == 0);
        /* a waiting task whose family timer fires completes timed out (FreeBSD's callout runs from submit) */
        f.mapShortage = 1000;
        FakeTask *c = f.NewRead(t9, 2000);
        CHECK(f.Submit(c) == SAS2308Glue::kInProcess);
        f.Run(2100);
        CHECK(c->completions == 1 && c->done == SAS2308GluePort::kDoneTimeout && !c->mapped);
        /* an abort of a waiting task completes it; a stop completes the rest (DeviceNotPresent) */
        FakeTask *d = f.NewRead(t9), *e = f.NewRead(t9);
        CHECK(f.Submit(d) == SAS2308Glue::kInProcess && f.Submit(e) == SAS2308Glue::kInProcess);
        CHECK(f.glue.AbortWaiting(d) && d->completions == 1 && d->done == SAS2308GluePort::kDoneRetry);
        CHECK(!f.glue.AbortWaiting(d));
        f.mapShortage = 0;
        f.ioc->holdIo = true;
        f.glue.StopCore();
        CHECK(e->completions == 1 && e->done == SAS2308GluePort::kDoneGone);
        f.Detach();
    }

    if (Want("D46")) {
        /* the task attribute reaches the IOC as the request's queue type (da passes its tag_action) */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const struct { int attr; uint32_t control; } cases[] = {
            {MPSK_ATTR_SIMPLE, MPI2_SCSIIO_CONTROL_SIMPLEQ}, {MPSK_ATTR_ORDERED, MPI2_SCSIIO_CONTROL_ORDEREDQ},
            {MPSK_ATTR_HEAD, MPI2_SCSIIO_CONTROL_HEADOFQ}, {MPSK_ATTR_ACA, MPI2_SCSIIO_CONTROL_ACAQ},
        };
        for (const auto &c : cases) {
            FakeTask *t = f.NewRead(f.Tid(0x0009));
            t->attr = c.attr;
            CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            CHECK((m.lastControl & MPI2_SCSIIO_CONTROL_TASKATTRIBUTE_MASK) == c.control);
            HostRunPending();
            CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus);
        }
        f.Detach();
    }

    if (Want("D46-otag")) {
        /*
         * da's periodic ORDERED tag: under continuous IO with SIMPLE tags
         * only, one READ goes ORDERED per two 15 s intervals; none when
         * idle, none with send_ordered 0; an ORDERED task from the OS
         * counts as the interval's (DA_FLAG_WAS_OTAG).
         */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        /* one READ a second for `secs' s, all kept by the firmware (pending throughout); ORDERED ones' times */
        auto load = [&](int secs, int firstAttr) {
            std::vector<uint64_t> ordered;
            m.holdIo = true;
            for (int i = 0; i < secs; i++) {
                FakeTask *t = f.NewRead(t9, 600000);
                t->attr = i == 0 ? firstAttr : MPSK_ATTR_SIMPLE;
                CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
                if ((m.lastControl & MPI2_SCSIIO_CONTROL_TASKATTRIBUTE_MASK) == MPI2_SCSIIO_CONTROL_ORDEREDQ && i > 0)
                    ordered.push_back(m.nowUs / 1000000);
                f.Run(1000);
            }
            m.holdIo = false;
            m.ReleasePending();
            HostRunPending();
            return ordered;
        };
        const uint32_t before = gHost.sc->xnu.stats.ordered_tags;
        std::vector<uint64_t> o = load(62, MPSK_ATTR_SIMPLE);
        printf("  ORDERED at %s s\n", [&] { std::string r; for (uint64_t x : o) r += std::to_string(x) + " "; return r; }().c_str());
        CHECK(o.size() == 2);                       /* intervals 1 and 3 of 4: one per two intervals */
        CHECK(o.size() == 2 && o[1] - o[0] >= 29 && o[1] - o[0] <= 31);
        CHECK(gHost.sc->xnu.stats.ordered_tags == before + 2);
        /* idle (nothing pending at the ticks), then one READ: SIMPLE */
        f.Run(40000);
        FakeTask *one = f.NewRead(t9);
        CHECK(f.Submit(one) == SAS2308Glue::kInProcess);
        CHECK((m.lastControl & MPI2_SCSIIO_CONTROL_TASKATTRIBUTE_MASK) == MPI2_SCSIIO_CONTROL_SIMPLEQ);
        HostRunPending();
        CHECK(one->completions == 1);
        /* an OS ORDERED task first: the next interval needs none, the one after does */
        f.Run(15000);
        o = load(32, MPSK_ATTR_ORDERED);
        CHECK(o.size() == 1);
        /* kern.cam.da.send_ordered 0 */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SEND_ORDERED, 0) == 0 && mps_kext_knob(f.k, MPSK_KNOB_SEND_ORDERED) == 0);
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SEND_ORDERED, 2) != 0);
        f.Run(15000);                               /* a NEED set before the change is spent first, as da */
        o = load(62, MPSK_ATTR_SIMPLE);
        CHECK(o.empty());
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SEND_ORDERED, 1) == 0);
        f.Detach();                                 /* the callout is drained before the state goes (ASan) */
    }

    if (Want("S1-spindown")) {
        /*
         * sas2308_idle_spindown 0: SAM's idle stop (START STOP UNIT, IMMED 1,
         * START 0) completes with GOOD without reaching the IOC; START 1,
         * LOEJ, a POWER CONDITION, the knob at 1, the system going to sleep,
         * an unknown system state and the halt all send it.
         */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        /* true: the command reached the IOC; the task completed GOOD either way */
        auto ssu = [&](uint8_t b1, uint8_t b4) {
            FakeTask *t = f.NewRead(t9, 0);
            memset(t->cdb, 0, sizeof(t->cdb));
            t->cdb[0] = 0x1B; t->cdb[1] = b1; t->cdb[4] = b4; t->cdbLen = 6; t->dir = MPSK_DIR_NONE; t->len = 0;
            const size_t ops = m.scsiOps.size();
            CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            HostRunPending();
            CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
            return m.scsiOps.size() != ops;
        };
        const uint32_t before = gHost.sc->xnu.stats.idle_stops;
        CHECK(mps_kext_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN) == 0);       /* the default (EJ) */
        CHECK(ssu(0x01, 0x00));                     /* system state unknown (no priority interest): sent */
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        CHECK(!ssu(0x01, 0x00));                    /* SAM's Standby / Sleep stop: completed here */
        CHECK(!ssu(0x00, 0x00));                    /* IMMED 0 */
        CHECK(!ssu(0x01, 0x04));                    /* NO_FLUSH: still a stop */
        CHECK(gHost.sc->xnu.stats.idle_stops == before + 3);
        CHECK(ssu(0x00, 0x01));                     /* START 1 (SAM's Active) */
        CHECK(ssu(0x01, 0x02));                     /* LOEJ (eject) */
        CHECK(ssu(0x01, 0x50));                     /* POWER CONDITION 5 (SAM's system sleep) */
        CHECK(ssu(0x01, 0x30));                     /* POWER CONDITION 3 (STANDBY) */
        CHECK(gHost.sc->xnu.stats.idle_stops == before + 3);
        /* the read path is untouched */
        FakeTask *rd = f.NewRead(t9);
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(rd->completions == 1 && rd->done == SAS2308GluePort::kDoneStatus);
        /* the knob: 1 sends it, 3 is refused (2: S2) */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 1) == 0 && mps_kext_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN) == 1);
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 3) != 0);
        CHECK(ssu(0x01, 0x00));
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 0) == 0);
        CHECK(!ssu(0x01, 0x00));
        /* going to sleep (kIOMessageSystemWillSleep) until the wake: sent */
        mps_kext_system_power(f.k, MPSK_SYS_SLEEP);
        CHECK(ssu(0x01, 0x00));
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        CHECK(!ssu(0x01, 0x00));
        CHECK(gHost.sc->xnu.stats.idle_stops == before + 5);
        /* the halt (restart / power off, batch F): sent */
        mps_kext_halt_begin(f.k);
        CHECK(ssu(0x01, 0x00));
        CHECK(gHost.sc->xnu.stats.idle_stops == before + 5);
        f.Detach();
    }

    if (Want("S2-in-use")) {
        /*
         * sas2308_idle_spindown 2 (the default): SAM's idle stop is sent
         * when the disk is not in use, completed here when it is (a mounted
         * volume, an open /dev/(r)disk: the class's TargetInUse). The disk
         * is asked only for an idle stop in mode 2; modes 0 and 1 and the
         * sleep / halt rules are as S1.
         */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009), ta = f.Tid(0x000A);
        auto ssu = [&](uint32_t tid, uint8_t b1, uint8_t b4) {
            FakeTask *t = f.NewRead(tid, 0);
            memset(t->cdb, 0, sizeof(t->cdb));
            t->cdb[0] = 0x1B; t->cdb[1] = b1; t->cdb[4] = b4; t->cdbLen = 6; t->dir = MPSK_DIR_NONE; t->len = 0;
            const size_t ops = m.scsiOps.size();
            CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            HostRunPending();
            CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
            return m.scsiOps.size() != ops;
        };
        struct mps_xnu_stats &st = gHost.sc->xnu.stats;
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 2) == 0);  /* not the default (EJ: 0) */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SPINDOWN_GRACE, 0) == 0);  /* no boot grace (S2-grace) */
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        f.inUse = {t9};                             /* t9 mounted, ta not */
        const uint32_t local0 = st.idle_stops, sent0 = st.idle_stops_sent;
        int calls = f.inUseCalls;
        CHECK(!ssu(t9, 0x01, 0x00));                /* in use: completed here */
        CHECK(f.inUseCalls == calls + 1);
        CHECK(st.idle_stops == local0 + 1 && st.idle_stops_sent == sent0);
        CHECK(ssu(ta, 0x01, 0x00));                 /* not in use: sent */
        CHECK(f.inUseCalls == calls + 2);
        CHECK(st.idle_stops == local0 + 1 && st.idle_stops_sent == sent0 + 1);
        CHECK(m.scsiOps.back().op == 0x1B && (m.scsiOps.back().b4 & 0x01) == 0);
        /* not an idle stop: sent, and the disk is not asked */
        calls = f.inUseCalls;
        CHECK(ssu(t9, 0x00, 0x01));                 /* START 1 */
        CHECK(ssu(t9, 0x01, 0x50));                 /* POWER CONDITION 5 (system sleep) */
        FakeTask *rd = f.NewRead(t9);
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(rd->completions == 1 && rd->done == SAS2308GluePort::kDoneStatus);
        CHECK(f.inUseCalls == calls);
        /* unmounted now: the next idle stop is sent */
        f.inUse.clear();
        CHECK(ssu(t9, 0x01, 0x00));
        f.inUse = {t9};
        /* going to sleep: sent though in use (S1's rule) */
        mps_kext_system_power(f.k, MPSK_SYS_SLEEP);
        CHECK(ssu(t9, 0x01, 0x00));
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        CHECK(!ssu(t9, 0x01, 0x00));
        /* modes 0 and 1: the disk is not asked */
        calls = f.inUseCalls;
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 0) == 0);
        CHECK(!ssu(ta, 0x01, 0x00));                /* not in use, completed here all the same */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 1) == 0);
        CHECK(ssu(t9, 0x01, 0x00));                 /* in use, sent all the same */
        CHECK(f.inUseCalls == calls);
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 2) == 0);
        /* the halt: sent */
        mps_kext_halt_begin(f.k);
        CHECK(ssu(t9, 0x01, 0x00));
        f.Detach();
    }

    if (Want("S2-grace")) {
        /*
         * sas2308_spindown_grace (default 900 s): in mode 2, an idle stop
         * within that time of the target's nub creation is completed here
         * without asking whether the disk is in use (SAM stops every disk
         * about a minute after boot, before the GUI login mounts its
         * volumes); counted in idle_stops_grace too. 0: no grace. Mode 0
         * and 1 and the sleep rule do not look at it.
         */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        auto ssu = [&](uint32_t tid) {
            FakeTask *t = f.NewRead(tid, 0);
            memset(t->cdb, 0, sizeof(t->cdb));
            t->cdb[0] = 0x1B; t->cdb[1] = 0x01; t->cdbLen = 6; t->dir = MPSK_DIR_NONE; t->len = 0;
            const size_t ops = m.scsiOps.size();
            CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            HostRunPending();
            CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
            return m.scsiOps.size() != ops;
        };
        struct mps_xnu_stats &st = gHost.sc->xnu.stats;
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 2) == 0);  /* not the default (EJ: 0) */
        CHECK(mps_kext_knob(f.k, MPSK_KNOB_SPINDOWN_GRACE) == 900);    /* the default, seconds */
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        /* just attached, not in use: completed here, the disk not asked */
        int calls = f.inUseCalls;
        const uint32_t local0 = st.idle_stops, grace0 = st.idle_stops_grace, sent0 = st.idle_stops_sent;
        CHECK(!ssu(t9));
        CHECK(f.inUseCalls == calls);
        CHECK(st.idle_stops == local0 + 1 && st.idle_stops_grace == grace0 + 1 && st.idle_stops_sent == sent0);
        /* grace 0: the in-use rule at once */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SPINDOWN_GRACE, 0) == 0);
        CHECK(ssu(t9));
        CHECK(f.inUseCalls == calls + 1 && st.idle_stops_sent == sent0 + 1 && st.idle_stops_grace == grace0 + 1);
        /* the grace counts from the nub's creation: 3 s later, 2 s of grace are over, 4 s are not */
        f.Run(3000);
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SPINDOWN_GRACE, 2) == 0);
        CHECK(ssu(t9));
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SPINDOWN_GRACE, 4) == 0);
        CHECK(!ssu(t9));
        CHECK(st.idle_stops_grace == grace0 + 2);
        /* a disk that arrives now has its own grace */
        m.devices.push_back(0x000A);
        m.PostTopologyEvent({0x000A}, MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED);
        HostRunPending();
        const uint32_t ta = f.Tid(0x000A);
        CHECK(f.nubs.count(ta));
        f.Run(1000);
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_SPINDOWN_GRACE, 2) == 0);
        CHECK(ssu(t9));                             /* over for t9 */
        CHECK(!ssu(ta));                            /* not for ta */
        /* in use within the grace: a grace stop (the disk not asked) */
        f.inUse = {ta};
        calls = f.inUseCalls;
        CHECK(!ssu(ta));
        CHECK(f.inUseCalls == calls && st.idle_stops_grace == grace0 + 4);
        /* modes 0 and 1: no grace stops */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 0) == 0);
        CHECK(!ssu(ta));
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 1) == 0);
        CHECK(ssu(ta));
        CHECK(st.idle_stops_grace == grace0 + 4);
        /* going to sleep within the grace: sent */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 2) == 0);
        mps_kext_system_power(f.k, MPSK_SYS_SLEEP);
        CHECK(ssu(ta));
        CHECK(st.idle_stops_grace == grace0 + 4);
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        f.Detach();
    }

    if (Want("EJ-eject")) {
        /*
         * Eject spins the disk down (EJ): SAM's idle-type stop (START 0,
         * LOEJ 0, POWER CONDITION 0) is sent in every mode when macOS is
         * ejecting / has ejected the disk's media (the class's
         * TargetEjected: the block storage driver open by DKIOCEJECT, or no
         * media under it), counted in idle_stops_ejected. The default mode
         * is 0 again (never for idleness). Mode 2's grace and in-use rules
         * come after it; mode 1 sends every stop without asking. Sleep, the
         * halt: sent as before, not counted.
         */
        MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009), ta = f.Tid(0x000A);
        auto ssu = [&](uint32_t tid, uint8_t b1, uint8_t b4) {
            FakeTask *t = f.NewRead(tid, 0);
            memset(t->cdb, 0, sizeof(t->cdb));
            t->cdb[0] = 0x1B; t->cdb[1] = b1; t->cdb[4] = b4; t->cdbLen = 6; t->dir = MPSK_DIR_NONE; t->len = 0;
            const size_t ops = m.scsiOps.size();
            CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            HostRunPending();
            CHECK(t->completions == 1 && t->done == SAS2308GluePort::kDoneStatus && t->scsiStatus == 0);
            return m.scsiOps.size() != ops;
        };
        struct mps_xnu_stats &st = gHost.sc->xnu.stats;
        CHECK(mps_kext_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN) == 0);       /* the default again */
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        const uint32_t local0 = st.idle_stops, ej0 = st.idle_stops_ejected;
        /* mode 0: the ejected disk's stop is sent, the other's completed here */
        f.ejected = {t9};
        int calls = f.ejectCalls;
        CHECK(ssu(t9, 0x01, 0x00));
        CHECK(f.ejectCalls == calls + 1);
        CHECK(st.idle_stops_ejected == ej0 + 1 && st.idle_stops == local0);
        CHECK(m.scsiOps.back().handle == 0x0009 && m.scsiOps.back().op == 0x1B && (m.scsiOps.back().b4 & 0x03) == 0);
        CHECK(!ssu(ta, 0x01, 0x00));
        CHECK(f.ejectCalls == calls + 2);
        CHECK(st.idle_stops_ejected == ej0 + 1 && st.idle_stops == local0 + 1);
        /* not an idle stop: the disk is not asked */
        calls = f.ejectCalls;
        CHECK(ssu(t9, 0x00, 0x01));                 /* START 1 */
        CHECK(ssu(t9, 0x01, 0x50));                 /* POWER CONDITION 5 */
        FakeTask *rd = f.NewRead(t9);
        CHECK(f.Submit(rd) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(rd->completions == 1 && rd->done == SAS2308GluePort::kDoneStatus);
        CHECK(f.ejectCalls == calls);
        /* mounted again (media back): completed here */
        f.ejected.clear();
        CHECK(!ssu(t9, 0x01, 0x00));
        CHECK(st.idle_stops_ejected == ej0 + 1);
        /* mode 2: ejected wins over the grace (default 900 s, just attached) and over in use */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 2) == 0);
        CHECK(mps_kext_knob(f.k, MPSK_KNOB_SPINDOWN_GRACE) == 900);
        const uint32_t grace0 = st.idle_stops_grace, sent0 = st.idle_stops_sent;
        f.ejected = {t9};
        f.inUse = {t9};
        CHECK(ssu(t9, 0x01, 0x00));
        CHECK(st.idle_stops_ejected == ej0 + 2 && st.idle_stops_grace == grace0 && st.idle_stops_sent == sent0);
        CHECK(!ssu(ta, 0x01, 0x00));                /* not ejected: the grace as before */
        CHECK(st.idle_stops_grace == grace0 + 1);
        f.inUse.clear();
        /* mode 1: sent without asking, not counted */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 1) == 0);
        calls = f.ejectCalls;
        CHECK(ssu(t9, 0x01, 0x00));
        CHECK(ssu(ta, 0x01, 0x00));
        CHECK(f.ejectCalls == calls && st.idle_stops_ejected == ej0 + 2);
        /* going to sleep: sent, not counted as an eject */
        CHECK(mps_kext_set_knob(f.k, MPSK_KNOB_IDLE_SPINDOWN, 0) == 0);
        mps_kext_system_power(f.k, MPSK_SYS_SLEEP);
        CHECK(ssu(t9, 0x01, 0x00));
        CHECK(st.idle_stops_ejected == ej0 + 2);
        mps_kext_system_power(f.k, MPSK_SYS_AWAKE);
        /* the halt: sent, not counted */
        mps_kext_halt_begin(f.k);
        CHECK(ssu(t9, 0x01, 0x00));
        CHECK(st.idle_stops_ejected == ej0 + 2);
        f.Detach();
    }

    if (Want("D21")) {
        /*
         * The family keeps every OS task busy (each completion resubmitted
         * at once, or the IOC keeps them all) while a disk is hot-added
         * (SAS Device Page 0 and SATA IDENTIFY need a normal frame, one at
         * a time on the event queue) and another is removed (target reset
         * and REMOVE_DEVICE use a high-priority frame). With FreeBSD's
         * sizing (CAM gets every normal frame) the add is given up after
         * every retry; one frame kept from the OS is enough (8 were kept
         * before, D58). The removal works either way.
         */
        auto run = [&](int reserve, bool stuck) {   /* reserve < 0: the kext's own sizing */
            MockIocModel m; m.devices = {0x0009, 0x000A}; m.maxTargets = 32; m.strictEventMasks = false;
            m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
            Family f; f.Attach(&m);
            struct mps_softc *sc = gHost.sc;
            if (reserve >= 0)
                f.glue.fMaxTasks = (uint32_t)(sc->num_reqs - sc->num_prireqs - 1 - reserve);
            const uint32_t t9 = f.Tid(0x0009), tA = f.Tid(0x000A);
            bool refill = true;
            f.onComplete = [&](FakeTask *t) {      /* the same task again (the family's pool) */
                if (!refill || t->target != t9) return;
                t->completions = 0;
                CHECK(f.Submit(t) == SAS2308Glue::kInProcess);
            };
            m.holdIo = true;
            for (uint32_t i = 0; i < f.glue.fMaxTasks; i++)
                CHECK(f.Submit(f.NewRead(t9, 600000)) == SAS2308Glue::kInProcess);
            m.devices = {0x0009, 0x000B};
            m.PostTopologyEvent({0x000B});
            m.PostTopologyEvent({0x000A}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
            for (int ms = 0; ms < 45000; ms += 10) {    /* longer than the page retries and 8 re-adds */
                HostAdvance(10);
                if (!stuck) m.ReleasePending();     /* every IO answered within 10 ms, then refilled */
                HostRunPending();
            }
            refill = false;
            m.holdIo = false;
            m.ReleasePending();
            f.Run(30000);
            uint32_t tB = ~0u;
            for (u_int i = 0; i < sc->sassc->maxtargets; i++)
                if (sc->sassc->targets[i].handle == 0x000B) tB = i;
            const bool added = tB != ~0u && f.nubs.count(tB), removed = !f.nubs.count(tA) && m.removeDevices == 1;
            printf("  %d OS tasks of %d normal frames, %s: add %s (page retries %u, re-adds %u, given up %u), "
                   "removal %s\n", (int)f.glue.fMaxTasks, sc->num_reqs - sc->num_prireqs - 1,
                   stuck ? "IOC keeps all" : "refilled at completion", added ? "ok" : "LOST",
                   sc->xnu.stats.config_retries, sc->xnu.stats.readd_scheduled, sc->xnu.stats.readd_given_up,
                   removed ? "ok" : "LOST");
            CHECK(removed);
            f.Detach();
            return added;
        };
        CHECK(run(-1, false));                      /* the kext's sizing: the add works */
        CHECK(run(-1, true));
        CHECK(!run(0, true));                       /* FreeBSD's: lost (the evidence for the one frame) */
    }

    if (Want("debug")) {
        /* the hardware-test triggers: a hold longer than the timeout (re-armed, D4) and a lost command */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        CHECK(mps_kext_debug(f.k, MPSK_DEBUG_HOLD, 35000) == 0);
        FakeTask *a = f.NewRead(t9);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess && f.glue.fKStats.tasksHeld == 1);
        f.Run(40000);
        CHECK(a->completions == 1 && a->done == SAS2308GluePort::kDoneStatus && a->timeouts == 0);
        CHECK(gHost.sc->xnu.stats.io_timeouts == 0 && m.tms.empty());    /* held: not timed (D4) */
        CHECK(mps_kext_debug(f.k, MPSK_DEBUG_LOSE, 1) == 0);
        FakeTask *b = f.NewRead(t9);
        CHECK(f.Submit(b) == SAS2308Glue::kInProcess);
        f.Run(29000);
        CHECK(b->completions == 0);
        f.Run(10000);                               /* timeout, abort (nothing to abort), escalation */
        CHECK(b->completions == 1 && gHost.sc->xnu.stats.io_timeouts == 1 && !m.tms.empty());
        CHECK(m.tms[0].type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK);
        const int resets = m.diagResetCount;
        CHECK(mps_kext_debug(f.k, MPSK_DEBUG_REINIT, 1) == 0);
        f.Run(3000);
        CHECK(m.diagResetCount == resets + 1);
        FakeTask *c = f.NewRead(t9);
        CHECK(f.Submit(c) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(c->completions == 1 && c->done == SAS2308GluePort::kDoneStatus);
        f.Detach();
    }

    if (Want("D23")) {
        /* the kext adds no second line to the core's (refused IO, resets) */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f; f.Attach(&m);
        const uint32_t t9 = f.Tid(0x0009);
        auto count = [](size_t mark, const char *sub) {
            int n = 0;
            for (size_t i = mark; i < gHost.log.size(); i++) n += gHost.log[i].line.find(sub) != std::string::npos;
            return n;
        };
        auto cls = [](size_t mark, const char *sub) {
            for (size_t i = mark; i < gHost.log.size(); i++)
                if (gHost.log[i].line.find(sub) != std::string::npos) return gHost.log[i].cls;
            return 0u;
        };
        /* refused by mpssas_action_scsiio (no device behind the id): the core's line only */
        size_t mark = gHost.Mark();
        FakeTask *a = f.NewRead(t9 + 5);
        CHECK(f.Submit(a) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(a->completions == 1 && a->done == SAS2308GluePort::kDoneGone);
        CHECK(count(mark, "NULL handle for target") == 1 && count(mark, "refused (CAM status") == 0);
        /* refused before the core (another disk behind the id): the kext's line, xinfo */
        mps_lock(gHost.sc); gHost.sc->xnu.tids[t9].gen++; mps_unlock(gHost.sc);
        mark = gHost.Mark();
        FakeTask *b = f.NewRead(t9);
        CHECK(f.Submit(b) == SAS2308Glue::kInProcess);
        HostRunPending();
        mps_lock(gHost.sc); gHost.sc->xnu.tids[t9].gen--; mps_unlock(gHost.sc);
        CHECK(b->done == SAS2308GluePort::kDoneGone && cls(mark, "refused (CAM status") == MPS_XINFO);
        /* a timeout racing the completion: xinfo */
        FakeTask *c = f.NewRead(t9);
        CHECK(f.Submit(c) == SAS2308Glue::kInProcess);
        HostRunPending();
        CHECK(c->completions == 1);
        mark = gHost.Mark();
        f.glue.HandleTimeout(c);                    /* its completion went first: not outstanding */
        CHECK(c->completions == 1 && cls(mark, "timeout for a task that is not outstanding") == MPS_XINFO);
        /* a reinit and a target reset: the core's lines, none from the kext's async handler */
        mark = gHost.Mark();
        RequestReinit("D23");
        f.Run(3000);
        CHECK(count(mark, "Reinitializing controller") == 1);
        CHECK(count(mark, "targets wait for the rediscovery") == 0);
        f.Detach();
    }

    if (Want("D25")) {
        /* lines without a device (printf, KASSERT) go where the host's go, with its unit */
        MockIocModel m; m.devices = {0x0009}; m.maxTargets = 32; m.strictEventMasks = false;
        m.tmSmidLo = 1; m.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        Family f;
        f.host.unit = 3;
        f.host.use_iolog = 1;
        f.Attach(&m);
        const struct mps_kext_host *h = mps_kext_log_host(nullptr);
        CHECK(h == &f.host && h->unit == 3 && h->use_iolog == 1);
        struct mps_kext_host other{};
        CHECK(mps_kext_log_host(&other) == &other);  /* a line with a device keeps its own */
        f.Detach();
        CHECK(mps_kext_log_host(nullptr) == nullptr);
    }

    printf("PASS (%d checks)\n", gChecks);
    return 0;
}
