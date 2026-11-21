/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * SAS2308Glue.cpp - see SAS2308Glue.h. No IOKit: built into the kext and
 * into test/kext_glue_test.
 */
#include "SAS2308Glue.h"

#include <stdarg.h>
#include <string.h>
#if defined(KERNEL)
#include <libkern/libkern.h>    /* vsnprintf */
#else
#include <stdio.h>
#endif

bool
SAS2308Glue::Init(SAS2308GluePort *port)
{
    fPort = port;
    if (fLock == nullptr)
        fLock = mps_kext_lock_alloc("SAS2308Glue");
    return fLock != nullptr;
}

void
SAS2308Glue::Fini(void)
{
    uint32_t n = 0;
    SAS2308TargetTable::Entry *e = fTargets.Release(&n);
    if (e)
        mps_kext_zfree(e, sizeof(*e) * n);
    fMaxTasks = 0;
    fMaxTargets = 0;
}

void
SAS2308Glue::FreeLock(void)
{
    if (fLock != nullptr) {
        mps_kext_lock_free(fLock);
        fLock = nullptr;
    }
}

struct mps_kext *
SAS2308Glue::TakeCore(void)
{
    if (!fLock) {
        struct mps_kext *m = fMps;
        fMps = nullptr;
        return m;
    }
    Lock();
    struct mps_kext *m = fMps;
    fMps = nullptr;
    Unlock();
    return m;
}

/*
 * The core's counters: under mps_mtx while attached (the gate); after
 * mps_free a struct copy under fLock, so TakeCore/mps_kext_destroy cannot
 * free the core under it (L7). The copy never takes mps_mtx (fLock is a
 * leaf under it).
 */
bool
SAS2308Glue::Snapshot(struct mps_kext_snapshot *snap)
{
    if (Enter()) {
        mps_kext_snapshot(fMps, snap);
        Leave();
        return true;
    }
    if (!fLock)
        return false;
    Lock();
    const bool ok = fMps != nullptr && !fCoreAttached && !fBusy && mps_kext_snapshot_detached(fMps, snap);
    Unlock();
    return ok;
}

void
SAS2308Glue::SetDebug(uint32_t mask)
{
    if (!fLock)
        return;
    Lock();
    if (fMps)
        mps_kext_set_debug(fMps, mask);         /* a plain store: no mps_mtx */
    Unlock();
}

void
SAS2308Glue::Klog(uint32_t cls, const char *fmt, ...)
{
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (fPort)
        fPort->Log(cls, line);
}

#if 0
#pragma mark -
#pragma mark The gate around the core
#endif

bool
SAS2308Glue::Enter(void)
{
    if (!fLock)
        return false;
    Lock();
    const bool ok = !fStopping && fCoreAttached && fMps;
    if (ok)
        fBusy++;
    Unlock();
    return ok;
}

void
SAS2308Glue::Leave(void)
{
    Lock();
    if (--fBusy == 0 && fStopping)
        mps_kext_wakeup(&fBusy);
    Unlock();
}

//-----------------------------------------------------------------------------
//	Attached - after mps_attach: the core takes calls; the SGL limit and the
//	task count (ReportMaximumTaskCount, AllocateSCSIParallelTasks right after
//	InitializeController) come from IOCFacts. A later reinit may resize the
//	core's queues; the family's pool stays as allocated, and IO beyond the
//	core's commands waits in the thin CAM. No per-task table (D45): a task's
//	glue state is in its HBA data, and the family finds a task by its nexus
//	(FindTaskForAddress). False: nothing usable (the caller detaches the core).
//-----------------------------------------------------------------------------

bool
SAS2308Glue::Attached(void)
{
    const uint32_t segs = mps_kext_max_segs(fMps);
    const uint32_t tasks = mps_kext_max_tasks(fMps);
    const uint32_t targets = mps_kext_max_targets(fMps);
    if (tasks == 0 || segs == 0 || targets == 0) {
        Klog(MPSK_ERROR | MPSK_INFO | MPSK_INIT, "no OS tasks (%u), segments (%u) or target IDs (%u)", tasks, segs, targets);
        return false;
    }
    /* the target table, one entry per ID: sized once, as the family reads ReportHighestSupportedDeviceID once */
    SAS2308TargetTable::Entry *e = nullptr;
    if (fTargets.Count() != targets) {
        e = (SAS2308TargetTable::Entry *)mps_kext_zalloc(sizeof(*e) * targets);
        if (!e) {
            Klog(MPSK_ERROR | MPSK_INFO | MPSK_INIT, "no memory for %u target IDs", targets);
            return false;
        }
    }
    Lock();
    uint32_t oldCount = 0;
    SAS2308TargetTable::Entry *old = e ? fTargets.Release(&oldCount) : nullptr;
    if (e)
        fTargets.Attach(e, targets);
    fCoreAttached = true;
    fMaxSegs = segs;
    fMaxTasks = tasks;
    fMaxTargets = targets;
    Unlock();
    if (old)
        mps_kext_zfree(old, sizeof(*old) * oldCount);
    return true;
}

void
SAS2308Glue::Start(void)
{
    Lock();
    fStarted = true;
    fStopping = false;
    fHotplugStop = false;
    fDoneStop = false;
    fTargets.Start();
    Unlock();
}

//-----------------------------------------------------------------------------
//	StopCore - no new core calls, TM waiters released, running calls
//	finished, then mps_free(): message unit reset (the IOC stops touching
//	our memory), every ccb completed once (CAM_DEV_NOT_THERE), DMA freed.
//	Idempotent.
//-----------------------------------------------------------------------------

void
SAS2308Glue::StopCore(void)
{
    if (!fLock)
        return;
    Lock();
    fStarted = false;
    fStopping = true;
    fTargets.Stop();
    const bool attached = fCoreAttached;
    Unlock();
    if (!attached)
        return;
    mps_kext_stop_waiters(fMps);
    Lock();
    while (fBusy)
        mps_kext_sleep(fLock, &fBusy);
    fCoreAttached = false;
    Unlock();
    FlushMapWait(kAllTargets, SAS2308GluePort::kDoneGone);     /* never reached the core (D47) */
    mps_kext_detach(fMps);
    DeliverCompletions();
}

#if 0
#pragma mark -
#pragma mark IO
#endif

//-----------------------------------------------------------------------------
//	MapTask - bus_dmamap_load_ccb() input (CAM_DATA_SG_PADDR): the family
//	owns the IODMACommand and its memory descriptor; we prepare() the range
//	and complete() it when the IO is done. kMapShort: prepare/gen failed
//	(no IOVA space now): the task waits and is mapped again later, as
//	busdma defers a load (EINPROGRESS) (D47). kMapBad: the transfer cannot
//	be described (too many segments, a bad segment): never will.
//-----------------------------------------------------------------------------

SAS2308Glue::MapResult
SAS2308Glue::MapTask(void *task, const SAS2308GluePort::TaskDesc &d, uint32_t *nsegsOut)
{
    struct mps_kext_task_glue *tg = mps_kext_task_glue(d.td);
    uint32_t nsegs = 0;

    *nsegsOut = 0;
    if (d.length == 0)
        return kMapOk;
    struct mps_kext_seg *segs = mps_kext_task_segs(d.td);
    const uint64_t maxSegBytes = mps_kext_max_seg_bytes();
    if (!fPort->TaskMapBegin(task, d.length))
        return kMapShort;
    tg->prepared = 1;

    uint64_t offset = 0;
    MapResult r = kMapOk;
    while (r == kMapOk && offset < d.length && nsegs < fMaxSegs) {
        struct mps_kext_seg chunk[16];
        uint32_t n = fMaxSegs - nsegs;
        if (n > 16)
            n = 16;
        if (!fPort->TaskMapNext(task, &offset, chunk, &n) || n == 0) {
            r = kMapShort;
            break;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (chunk[i].len == 0 || chunk[i].len > maxSegBytes) {
                r = kMapBad;
                break;
            }
            segs[nsegs++] = chunk[i];
        }
    }
    if (r == kMapOk && (offset != d.length || nsegs == 0))
        r = kMapBad;
    if (r != kMapOk) {
        if (r == kMapBad)
            Klog(MPSK_ERROR | MPSK_INFO, "cannot map IO for target %u: %u segments for %llu of %llu bytes (cap %u)",
                 d.target, nsegs, (unsigned long long)offset, (unsigned long long)d.length, fMaxSegs);
        fPort->TaskUnmap(task);
        tg->prepared = 0;
        return r;
    }
    *nsegsOut = nsegs;
    return kMapOk;
}

//-----------------------------------------------------------------------------
//	Post - a mapped task to mpssas_action_scsiio (xpt_action). The caller
//	holds the gate (fBusy).
//-----------------------------------------------------------------------------

void
SAS2308Glue::Post(void *task, const SAS2308GluePort::TaskDesc &d, uint32_t nsegs)
{
    Lock();
    const uint32_t gen = fTargets.Gen(d.target);   /* the nub's device (mps_kext_submit checks it) */
    Unlock();
    struct mps_kext_io_result res;
    const int rc = mps_kext_submit(fMps, d.td, task, d.tag, d.target, d.lun, d.cdb, d.cdbLen, d.dir, d.attr, nsegs,
                                   d.length, d.timeoutMs, gen, &res);
    Lock();
    if (rc == MPSK_SUBMIT_DONE) {
        fKStats.tasksRejected++;
        ScheduleDone();                 /* delivered from the done call: no family re-entry from here */
    } else {
        fKStats.tasks++;
        if (rc == MPSK_SUBMIT_HELD)
            fKStats.tasksHeld++;
    }
    Unlock();
    /*
     * Refused before the core (invalid request, not attached, another disk
     * behind the id). What mpssas_action_scsiio refuses it logs itself, as
     * FreeBSD (NULL handle, shutting down, cannot map), or not at all (a
     * target being removed): no second line (D23).
     */
    if (rc == MPSK_SUBMIT_DONE && !res.by_core)
        Klog(res.kind == MPSK_IO_REJECT ? (MPSK_ERROR | MPSK_INFO) : MPSK_XINFO,
             "IO for target %u refused (CAM status 0x%x)", d.target, res.cam_status);
}

//-----------------------------------------------------------------------------
//	ProcessTask - SCSIParallelTask -> XPT_SCSI_IO ccb (mpssas_action_scsiio
//	via xpt_action).
//-----------------------------------------------------------------------------

SAS2308Glue::Response
SAS2308Glue::ProcessTask(void *task)
{
    SAS2308GluePort::TaskDesc d;

    if (!fPort->TaskDescribe(task, &d) || d.target >= fMaxTargets)
        return kFailure;
    if (d.cdbLen == 0 || d.cdbLen > sizeof(d.cdb))
        return kRejected;
    /* D47: a direction with no data is a command without data, as FreeBSD takes dxfer_len 0 */
    if (d.length == 0)
        d.dir = MPSK_DIR_NONE;
    if (d.dir == MPSK_DIR_NONE)
        d.length = 0;
    else if (d.length > 0xFFFFFFFFull)
        return kRejected;

    /*
     * The ccb and its segment list live in the task's HBA data (sized by
     * ReportHBASpecificTaskDataSize()), not on the kernel stack: they stay
     * valid until the task completes, which a held IO needs.
     */
    if (!d.td || d.tdSize < mps_kext_task_size(fMps) || fMaxSegs == 0)
        return kFailure;
    struct mps_kext_task_glue *tg = mps_kext_task_glue(d.td);
    memset(tg, 0, sizeof(*tg));
    tg->task = task;
    tg->target = d.target;

    /* Tasks waiting for a mapping go first: this one waits behind them (order, as a frozen SIM queue). */
    Lock();
    const bool behind = fMapWaitHead != nullptr;
    Unlock();
    uint32_t nsegs = 0;
    const MapResult mr = behind ? kMapShort : MapTask(task, d, &nsegs);
    if (mr == kMapBad)
        return kFailure;

    /*
     * A pass through the gate. The family never has more than fMaxTasks
     * (ReportMaximumTaskCount) outstanding; the count is a check.
     */
    Lock();
    const bool open = !fStopping && fCoreAttached && fMps;
    const bool room = open && fOutstanding < fMaxTasks;
    if (room) {
        fOutstanding++;
        fBusy++;
    } else {
        fKStats.tasksRejected++;
    }
    Unlock();
    if (!room) {
        if (tg->prepared)
            fPort->TaskUnmap(task);
        tg->prepared = 0;
        if (open)
            Klog(MPSK_ERROR | MPSK_INFO, "more than %u tasks outstanding: rejecting one", fMaxTasks);
        return open ? kRejected : kFailure;
    }

    /*
     * Review C1: the family times only tasks given to SetTimeoutForTask
     * (SCSIParallelTimer; Apple's iSCSI HBA does the same in its
     * ProcessParallelTask); 0 = the task's own timeout. Once armed, only
     * CompleteParallelTask removes the timer entry, so from here on every
     * outcome, a synchronous refusal too, completes through it. HandleTimeout
     * re-arms it while the task waits outside the IOC (D4).
     */
    fPort->SetTimeoutForTask(task, 0);
    if (mr == kMapShort) {
        /*
         * D47: no IOVA space now. FreeBSD's busdma defers the load and
         * calls back when resources are free; we retry from the next
         * completion delivery and the 1 s timer. Timed from here, as
         * FreeBSD arms the ccb's callout before mps_map_command.
         */
        Lock();
        tg->mapwait = 1;
        if (fMapWaitTail)
            mps_kext_task_glue(fMapWaitTail)->next = d.td;
        else
            fMapWaitHead = d.td;
        fMapWaitTail = d.td;
        const bool first = fKStats.mapWaits++ == 0;
        ScheduleDone();                 /* a retry soon, from the done call */
        if (--fBusy == 0 && fStopping)
            mps_kext_wakeup(&fBusy);
        Unlock();
        if (first)
            Klog(MPSK_ERROR | MPSK_XINFO, "IO for target %u waits for a DMA mapping (no IOVA space); "
                 "counted as \"Tasks waiting for a mapping\", logged once", d.target);
        return kInProcess;
    }
    Post(task, d, nsegs);
    Lock();
    if (--fBusy == 0 && fStopping)
        mps_kext_wakeup(&fBusy);
    Unlock();
    return kInProcess;
}

//-----------------------------------------------------------------------------
//	Tasks waiting for a mapping (D47). RetryMapWait maps and posts them in
//	order until one is short again; one retrier at a time. TakeWaiting
//	removes one (fLock held): a waiting task is unlinked; one the retrier
//	holds is marked, and the retrier completes it instead of posting it.
//-----------------------------------------------------------------------------

void
SAS2308Glue::RetryMapWait(void)
{
    for (;;) {
        Lock();
        void *td = fMapWaitHead;
        if (!td || fMapRetrying || fStopping || !fCoreAttached || !fMps) {
            Unlock();
            return;
        }
        struct mps_kext_task_glue *tg = mps_kext_task_glue(td);
        fMapWaitHead = tg->next;
        if (!fMapWaitHead)
            fMapWaitTail = nullptr;
        tg->next = nullptr;
        tg->mapwait = 0;
        tg->retrying = 1;
        fMapRetrying = true;
        fBusy++;
        Unlock();

        void *task = tg->task;
        SAS2308GluePort::TaskDesc d;
        uint32_t nsegs = 0;
        MapResult mr = fPort->TaskDescribe(task, &d) && d.td == td ? MapTask(task, d, &nsegs) : kMapBad;
        if (mr == kMapOk && d.length == 0)
            d.dir = MPSK_DIR_NONE;

        Lock();
        const uint8_t cancel = tg->cancel;
        if (mr == kMapShort && !cancel) {
            tg->next = fMapWaitHead;            /* first in line again */
            fMapWaitHead = td;
            if (!fMapWaitTail)
                fMapWaitTail = td;
            tg->mapwait = 1;
        }
        Unlock();

        bool again = true;
        if (cancel || mr == kMapBad) {
            if (tg->prepared)
                fPort->TaskUnmap(task);
            tg->prepared = 0;
            Lock();
            if (fOutstanding)
                fOutstanding--;
            Unlock();
            fPort->CompleteTask(task, cancel ? cancel - 1 : (int)SAS2308GluePort::kDoneReject, 0, 0, nullptr, 0);
        } else if (mr == kMapOk) {
            Post(task, d, nsegs);
        } else {
            again = false;                      /* still short: the next completion or tick */
        }
        Lock();
        tg->retrying = 0;
        tg->cancel = 0;
        fMapRetrying = false;
        mps_kext_wakeup(&fMapRetrying);
        if (--fBusy == 0 && fStopping)
            mps_kext_wakeup(&fBusy);
        Unlock();
        if (!again)
            return;
    }
}

bool
SAS2308Glue::TakeWaiting(void *td, int done)
{
    struct mps_kext_task_glue *tg = mps_kext_task_glue(td);

    if (tg->retrying) {
        tg->cancel = (uint8_t)(done + 1);
        return false;                           /* the retrier completes it */
    }
    if (!tg->mapwait)
        return false;
    void *prev = nullptr;
    for (void *p = fMapWaitHead; p; prev = p, p = mps_kext_task_glue(p)->next) {
        if (p != td)
            continue;
        if (prev)
            mps_kext_task_glue(prev)->next = tg->next;
        else
            fMapWaitHead = tg->next;
        if (fMapWaitTail == td)
            fMapWaitTail = prev;
        tg->next = nullptr;
        tg->mapwait = 0;
        if (fOutstanding)
            fOutstanding--;
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
//	AbortWaiting - AbortTaskRequest of a task that waits for its mapping:
//	completed (DeliveryFailure, like an aborted ccb). False: not waiting.
//-----------------------------------------------------------------------------

bool
SAS2308Glue::AbortWaiting(void *task)
{
    SAS2308GluePort::TaskDesc d;

    if (!fLock || !fPort->TaskDescribe(task, &d) || !d.td || mps_kext_task_glue(d.td)->task != task)
        return false;
    Lock();
    const bool retrying = mps_kext_task_glue(d.td)->retrying;
    const bool took = TakeWaiting(d.td, SAS2308GluePort::kDoneRetry);
    Unlock();
    if (took)
        fPort->CompleteTask(task, SAS2308GluePort::kDoneRetry, 0, 0, nullptr, 0);
    return took || retrying;
}

//-----------------------------------------------------------------------------
//	FlushMapWait - complete the waiting tasks of `target' (kAllTargets: all)
//	as `done', after a running retry has finished: before a nub is
//	destroyed, and at stop.
//-----------------------------------------------------------------------------

void
SAS2308Glue::FlushMapWait(uint32_t target, int done)
{
    if (!fLock)
        return;
    for (;;) {
        void *task = nullptr;
        Lock();
        while (fMapRetrying)
            mps_kext_sleep(fLock, &fMapRetrying);
        for (void *p = fMapWaitHead; p && !task; p = mps_kext_task_glue(p)->next) {
            const struct mps_kext_task_glue *tg = mps_kext_task_glue(p);
            if (target == kAllTargets || tg->target == target) {
                task = tg->task;
                (void)TakeWaiting(p, done);
            }
        }
        Unlock();
        if (!task)
            return;
        fPort->CompleteTask(task, done, 0, 0, nullptr, 0);
    }
}

//-----------------------------------------------------------------------------
//	HandleTimeout - mpssas_scsiio_timeout() through mps_xnu_ccb_timeout(),
//	asynchronous: the task completes (TaskTimeoutOccurred) when the IOC
//	gives it back. D4: a task still held, or posted less than its timeout
//	ago, is re-armed for the rest instead (FreeBSD times only the IOC's
//	ownership).
//-----------------------------------------------------------------------------

void
SAS2308Glue::HandleTimeout(void *task)
{
    SAS2308GluePort::TaskDesc d;
    uint32_t rearm = 0;

    if (!Enter()) {
        Klog(MPSK_RECOVERY | MPSK_INFO | MPSK_ERROR, "timeout while stopping: the stop path completes the task");
        return;
    }
    Lock();
    fKStats.familyTimeouts++;
    Unlock();
    if (!fPort->TaskDescribe(task, &d))
        d.td = nullptr;
    /* D47: still waiting for its mapping (FreeBSD's callout runs from mpssas_action_scsiio): timed out */
    if (d.td && mps_kext_task_glue(d.td)->task == task) {
        Lock();
        const bool retrying = mps_kext_task_glue(d.td)->retrying;
        const bool took = TakeWaiting(d.td, SAS2308GluePort::kDoneTimeout);
        Unlock();
        if (took || retrying) {
            Leave();
            Klog(MPSK_RECOVERY | MPSK_INFO | MPSK_ERROR, "target %u: timeout while waiting for a DMA mapping", d.target);
            if (took)
                fPort->CompleteTask(task, SAS2308GluePort::kDoneTimeout, 0, 0, nullptr, 0);
            return;
        }
    }
    const int rc = d.td ? mps_kext_timeout(fMps, d.td, task, d.timeoutMs, &rearm) : -1;
    Leave();
    /*
     * Re-armed before any completion of this task reaches the family:
     * HandleTimeout runs in the work loop gate, CompleteParallelTask
     * takes it (and removes the timer entry), and our own delivery comes
     * after this. The timer took the task off its list before calling us
     * (SCSIParallelTimer::GetExpiredTask), so this is a first arm again.
     */
    if (rc == 0 && rearm != 0)
        fPort->SetTimeoutForTask(task, rearm);
    if (rc != 0)        /* its completion is queued, not yet delivered: the normal race (D23) */
        Klog(MPSK_XINFO, "timeout for a task that is not outstanding, ignored");
    DeliverCompletions();
}

//-----------------------------------------------------------------------------
//	DeliverCompletions - mpssas_scsiio_complete() results to the family.
//	No lock held.
//-----------------------------------------------------------------------------

uint32_t
SAS2308Glue::DeliverCompletions(void)
{
    uint32_t n = 0;

    if (!fMps || !fLock)
        return 0;
    for (;; n++) {
        struct mps_kext_io_result res;
        void *task = mps_kext_next_done(fMps, &res);
        if (!task)
            break;

        struct mps_kext_task_glue *tg = mps_kext_task_glue(res.td);
        const bool prepared = tg->prepared;
        tg->prepared = 0;
        Lock();
        if (fOutstanding)
            fOutstanding--;
        Unlock();
        if (prepared)
            fPort->TaskUnmap(task);

        int done;
        switch (res.kind) {
        case MPSK_IO_STATUS:  done = SAS2308GluePort::kDoneStatus; break;
        case MPSK_IO_GONE:    done = SAS2308GluePort::kDoneGone; break;
        case MPSK_IO_TIMEOUT: done = SAS2308GluePort::kDoneTimeout; break;
        case MPSK_IO_REJECT:  done = SAS2308GluePort::kDoneReject; break;
        default:
            /*
             * Reset (CAM_SCSI_BUS_RESET: killed by the 4th reinit or sleep
             * in a row; the core re-sends it after the first 3, D20b), OS
             * abort (CAM_REQ_ABORTED), transport errors: not the device's
             * fault, a failure the upper layers retry.
             */
            done = SAS2308GluePort::kDoneRetry;
            break;
        }
        fPort->CompleteTask(task, done, res.scsi_status, res.transferred, res.sense, res.sense_len);
    }
    RetryMapWait();                     /* completions unmapped IOVA space (D47) */
    return n;
}

//-----------------------------------------------------------------------------
//	TimerDeliver - the statistics timer (1 s, work loop) delivers what is
//	on the done queue with no done call queued or running: every completion
//	is kicked (interrupt path, done call), so these are lost kicks. Counted
//	and logged once, so a missing kick shows up in tests and statistics
//	instead of costing a silent second of latency (D43).
//-----------------------------------------------------------------------------

void
SAS2308Glue::TimerDeliver(void)
{
    if (!fMps || !fLock)
        return;
    RetryMapWait();                     /* tasks waiting for a mapping (D47) */
    Lock();
    const bool kicked = fDonePending != 0;
    Unlock();
    if (kicked)
        return;                                     /* the done call delivers them */
    const uint32_t n = DeliverCompletions();
    if (n == 0)
        return;
    Lock();
    const bool first = fKStats.lostKicks == 0;
    fKStats.lostKicks += n;
    Unlock();
    if (first)
        Klog(MPSK_ERROR, "%u completions were not kicked: delivered by the 1 s timer (counted as "
             "\"Lost completion kicks\", logged once)", n);
}

//-----------------------------------------------------------------------------
//	ScheduleDone / QuiesceDone - thread_call_cancel_wait() is only exported
//	by com.apple.kpi.private, so count the invocations that are queued or
//	running; Quiesce waits for zero. thread_call_enter() returns true if
//	the call was already queued: no additional invocation then.
//-----------------------------------------------------------------------------

void
SAS2308Glue::ScheduleDone(void)
{
    if (fDoneStop)
        return;
    if (!fPort->CallEnter(SAS2308GluePort::kCallDone))
        fDonePending++;
}

void
SAS2308Glue::DoneCallBody(void)
{
    DeliverCompletions();
    Lock();
    if (--fDonePending == 0)
        mps_kext_wakeup(&fDonePending);
    Unlock();
}

void
SAS2308Glue::QuiesceDone(void)
{
    if (!fLock)
        return;
    Lock();
    fDoneStop = true;
    if (fPort->CallCancel(SAS2308GluePort::kCallDone) && fDonePending)
        fDonePending--;                             /* was queued, will not run */
    while (fDonePending)
        mps_kext_sleep(fLock, &fDonePending);
    Unlock();
}

#if 0
#pragma mark -
#pragma mark Targets: discovery events and the hotplug thread
#endif

//-----------------------------------------------------------------------------
//	Async - CAM async events (mps_mtx held). `present' was sampled
//	(mps_xnu_target_present) when the event happened; the hotplug thread
//	acts on it later. AC_BUS_RESET alone changes nothing (targets survive a
//	reinit if the rediscovery finds them again). AC_BUS_RESET and
//	AC_SENT_BDR are not logged: the core logs the reinit and the TM (D23).
//-----------------------------------------------------------------------------

void
SAS2308Glue::Async(int event, uint32_t tid, const uint8_t *present, const uint32_t *gens, uint32_t count)
{
    if (event == MPSK_ASYNC_BUS_RESET || event == MPSK_ASYNC_SENT_BDR)
        return;
    if (!fLock)
        return;
    Lock();
    bool work = false;
    const uint32_t n = count < fTargets.Count() ? count : fTargets.Count();
    for (uint32_t i = (tid == MPSK_ALL_TARGETS ? 0 : tid); i < n; i++) {
        work |= fTargets.Update(i, present[i] != 0, gens ? &gens[i] : nullptr);
        if (tid != MPSK_ALL_TARGETS)
            break;
    }
    if (work)
        ScheduleHotplug();
    Unlock();
}

void
SAS2308Glue::ScheduleHotplug(void)
{
    if (fHotplugStop)
        return;
    if (!fPort->CallEnter(SAS2308GluePort::kCallHotplug))
        fHotplugPending++;
}

//-----------------------------------------------------------------------------
//	HotplugCallBody - the hotplug thread call. xnu starts a second
//	invocation when the call is entered while it runs (e.g. a removal
//	during CreateTargetForID's probe); like the compat taskqueue's
//	tq_active (4d64411), only one runs the loop: a re-entered invocation
//	asks it to scan again and leaves (review H1).
//-----------------------------------------------------------------------------

void
SAS2308Glue::HotplugCallBody(void)
{
    Lock();
    if (fHotplugActive) {
        fHotplugAgain = true;
    } else {
        fHotplugActive = true;
        do {
            fHotplugAgain = false;
            Unlock();
            Hotplug();
            Lock();
        } while (fHotplugAgain && !fHotplugStop);
        fHotplugActive = false;
    }
    if (--fHotplugPending == 0)
        mps_kext_wakeup(&fHotplugPending);
    Unlock();
}

//-----------------------------------------------------------------------------
//	QuiesceHotplug - no new invocations; wait for the running one, which
//	finishes the CreateTargetForID()/DestroyTargetForID() it is in.
//-----------------------------------------------------------------------------

void
SAS2308Glue::QuiesceHotplug(void)
{
    if (!fLock)
        return;
    Lock();
    fHotplugStop = true;
    if (fPort->CallCancel(SAS2308GluePort::kCallHotplug) && fHotplugPending)
        fHotplugPending--;                          /* was queued, will not run */
    while (fHotplugPending)
        mps_kext_sleep(fLock, &fHotplugPending);
    Unlock();
}

void
SAS2308Glue::Hotplug(void)
{
    uint32_t cursor = 0;

    for (;;) {
        SAS2308TargetTable::Work w = { SAS2308TargetTable::kNone, 0, 0 };
        Lock();
        if (!fHotplugStop)
            w = fTargets.Next(cursor);
        Unlock();
        if (w.action == SAS2308TargetTable::kNone)
            break;

        bool ok = true;
        if (w.action == SAS2308TargetTable::kCreate) {
            ok = fPort->CreateTarget(w.targetID);
        } else {
            /*
             * Review M7: no completion may reach the nub after it is
             * destroyed: fail its held IO, let its posted IO come back
             * (the removal's target reset), deliver, then destroy. No
             * time limit (D14): the removal TM's timeout and the reinit
             * bound it.
             */
            FlushMapWait(w.targetID, SAS2308GluePort::kDoneGone);
            if (Enter()) {
                const int left = mps_kext_target_drain(fMps, w.targetID, kDrainReinitMs);
                Leave();
                DeliverCompletions();
                if (left)
                    Klog(MPSK_MAPPING | MPSK_INFO | MPSK_ERROR, "target %u: %d IOs still outstanding at its destroy",
                         w.targetID, left);
            }
            fPort->DestroyTarget(w.targetID);
            Klog(MPSK_MAPPING | MPSK_INFO, "target %u destroyed", w.targetID);
        }
        Lock();
        fTargets.Done(w, ok);
        Unlock();
    }
}

//-----------------------------------------------------------------------------
//	WillTerminate - our part of willTerminate, before the family's loop
//	destroys the nubs. A surprise removal (all-ones doorbell) disables the
//	controller now, so every posted and held ccb completes (DEV_NOT_THERE)
//	and is delivered here; then each target gets the review M7 flush and
//	drain (nothing should be left). FreeBSD: detach runs mps_free, which
//	completes every ccb before the SIM and its periphs go. Then no new
//	hotplug work, and wait for a CreateTargetForID in progress.
//-----------------------------------------------------------------------------

bool
SAS2308Glue::WillTerminate(void)
{
    bool present = true;

    if (Enter()) {
        present = mps_kext_check_present(fMps) != 0;
        if (!present)
            mps_kext_disable_now(fMps);
        Leave();
        if (!present)
            FlushMapWait(kAllTargets, SAS2308GluePort::kDoneGone);
        DeliverCompletions();
        for (uint32_t id = 0; !present && id < fMaxTargets; id++) {
            if (!Enter())
                break;
            const int left = mps_kext_target_drain(fMps, id, kDrainReinitMs);
            Leave();
            DeliverCompletions();
            if (left)
                Klog(MPSK_MAPPING | MPSK_INFO | MPSK_ERROR, "target %u: %d IOs still outstanding at termination", id, left);
        }
    }
    StopTargets();
    QuiesceHotplug();
    return present;
}

void
SAS2308Glue::StopTargets(void)
{
    if (!fLock)
        return;
    Lock();
    fTargets.Stop();
    Unlock();
}

//-----------------------------------------------------------------------------
//	TerminateTargets - after IOSCSIParallelInterfaceController::willTerminate:
//	its loop destroys IDs 0 .. familyLimit - 1 only, so destroy the highest
//	ID ourselves (DestroyTargetForID of a missing ID is a no-op).
//-----------------------------------------------------------------------------

void
SAS2308Glue::TerminateTargets(uint32_t familyLimit)
{
    if (!fLock)
        return;
    for (uint32_t i = familyLimit; i < fMaxTargets; i++) {
        Lock();
        const bool left = fTargets.TakeCreated(i);
        Unlock();
        if (left) {
            fPort->DestroyTarget(i);
            Klog(MPSK_MAPPING | MPSK_INFO, "target %u destroyed (missed by the family's willTerminate)", i);
        }
    }
    Lock();
    fTargets.ForgetAll();
    Unlock();
}
