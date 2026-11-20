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
    if (fSlots != nullptr) {
        mps_kext_zfree(fSlots, sizeof(Slot) * fMaxTasks);
        fSlots = nullptr;
        fMaxTasks = 0;
    }
    if (fLock != nullptr) {
        mps_kext_lock_free(fLock);
        fLock = nullptr;
    }
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
//	InitializeController) come from IOCFacts, one slot per task. A later
//	reinit may resize the core's queues; the family's pool stays as
//	allocated, and IO beyond the core's commands waits in the thin CAM.
//	False: no memory for the slots (the caller detaches the core).
//-----------------------------------------------------------------------------

bool
SAS2308Glue::Attached(void)
{
    const uint32_t segs = mps_kext_max_segs(fMps);
    const uint32_t tasks = mps_kext_max_tasks(fMps);
    Slot *slots = tasks ? (Slot *)mps_kext_zalloc(sizeof(Slot) * tasks) : nullptr;
    if (!slots) {
        Klog(MPSK_ERROR | MPSK_INIT, "no memory for %u task slots", tasks);
        return false;
    }
    Lock();
    Slot *old = fSlots;
    const uint32_t oldCount = fMaxTasks;
    fCoreAttached = true;
    fMaxSegs = segs;
    fMaxTasks = tasks;
    fSlots = slots;
    Unlock();
    if (old)
        mps_kext_zfree(old, sizeof(Slot) * oldCount);
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
    mps_kext_detach(fMps);
    DeliverCompletions();
}

#if 0
#pragma mark -
#pragma mark IO
#endif

//-----------------------------------------------------------------------------
//	ProcessTask - SCSIParallelTask -> XPT_SCSI_IO ccb (mpssas_action_scsiio
//	via xpt_action).
//-----------------------------------------------------------------------------

SAS2308Glue::Response
SAS2308Glue::ProcessTask(void *task)
{
    SAS2308GluePort::TaskDesc d;

    if (!fPort->TaskDescribe(task, &d) || d.target >= kMaxTargets)
        return kFailure;
    if (d.cdbLen == 0 || d.cdbLen > sizeof(d.cdb))
        return kRejected;
    if (d.dir == MPSK_DIR_NONE)
        d.length = 0;
    else if (d.length == 0 || d.length > 0xFFFFFFFFull)
        return kRejected;

    /*
     * The ccb and its segment list live in the task's HBA data (sized by
     * ReportHBASpecificTaskDataSize()), not on the kernel stack: they stay
     * valid until the task completes, which a held IO needs.
     */
    if (!d.td || d.tdSize < mps_kext_task_size() || fMaxSegs == 0)
        return kFailure;

    /*
     * bus_dmamap_load_ccb() input (CAM_DATA_SG_PADDR): the family owns the
     * IODMACommand and its memory descriptor; we prepare() the range and
     * complete() it when the IO is done.
     */
    uint32_t nsegs = 0;
    bool prepared = false;
    if (d.length) {
        struct mps_kext_seg *segs = mps_kext_task_segs(d.td);
        const uint64_t maxSegBytes = mps_kext_max_seg_bytes();
        if (!fPort->TaskMapBegin(task, d.length))
            return kFailure;
        prepared = true;

        uint64_t offset = 0;
        bool ok = true;
        while (ok && offset < d.length && nsegs < fMaxSegs) {
            struct mps_kext_seg chunk[16];
            uint32_t n = fMaxSegs - nsegs;
            if (n > 16)
                n = 16;
            if (!fPort->TaskMapNext(task, &offset, chunk, &n) || n == 0) {
                ok = false;
                break;
            }
            for (uint32_t i = 0; i < n; i++) {
                if (chunk[i].len == 0 || chunk[i].len > maxSegBytes) {
                    ok = false;
                    break;
                }
                segs[nsegs++] = chunk[i];
            }
        }
        if (!ok || offset != d.length || nsegs == 0) {
            Klog(MPSK_ERROR | MPSK_INFO, "cannot map IO for target %u: %u segments for %llu of %llu bytes (cap %u)",
                 d.target, nsegs, (unsigned long long)offset, (unsigned long long)d.length, fMaxSegs);
            fPort->TaskUnmap(task);
            return kFailure;
        }
    }

    /* A slot (for AbortTaskRequest) and a pass through the gate. */
    Slot *slot = nullptr;
    Lock();
    const bool open = !fStopping && fCoreAttached && fMps;
    if (open) {
        for (uint32_t i = 0; i < fMaxTasks && !slot; i++)
            if (!fSlots[i].task)
                slot = &fSlots[i];
    }
    if (slot) {
        slot->task = task;
        slot->prepared = prepared;
        slot->target = d.target;
        slot->lun = d.lun;
        slot->tag = d.tag;
        fBusy++;
    } else {
        fKStats.tasksRejected++;
    }
    Unlock();
    if (!slot) {
        if (prepared)
            fPort->TaskUnmap(task);
        if (open)
            Klog(MPSK_ERROR, "more than %u tasks outstanding: rejecting one", fMaxTasks);
        return open ? kRejected : kFailure;
    }

    /*
     * Review C1: the family times only tasks given to SetTimeoutForTask
     * (SCSIParallelTimer; Apple's iSCSI HBA does the same in its
     * ProcessParallelTask); 0 = the task's own timeout. Once armed, only
     * CompleteParallelTask removes the timer entry, so from here on every
     * outcome, a synchronous refusal too, completes through it.
     */
    fPort->SetTimeoutForTask(task);
    struct mps_kext_io_result res;
    const int rc = mps_kext_submit(fMps, d.td, task, d.tag, d.target, d.lun, d.cdb, d.cdbLen, d.dir, nsegs,
                                   d.length, d.timeoutMs, &res);

    Lock();
    if (rc == MPSK_SUBMIT_DONE) {
        fKStats.tasksRejected++;
        ScheduleDone();                 /* delivered from the done call: no family re-entry from here */
    } else {
        fKStats.tasks++;
        if (rc == MPSK_SUBMIT_HELD)
            fKStats.tasksHeld++;
    }
    if (--fBusy == 0 && fStopping)
        mps_kext_wakeup(&fBusy);
    Unlock();

    if (rc == MPSK_SUBMIT_DONE)
        /* refused by mpssas_action_scsiio (unknown target, gone, invalid, too big) */
        Klog(res.kind == MPSK_IO_REJECT ? MPSK_ERROR : (MPSK_ERROR | MPSK_XINFO),
             "IO for target %u refused (CAM status 0x%x)", d.target, res.cam_status);
    return kInProcess;
}

//-----------------------------------------------------------------------------
//	HandleTimeout - mpssas_scsiio_timeout() through mps_xnu_ccb_timeout(),
//	asynchronous: the task completes (ProtocolTimeoutOccurred) when the IOC
//	gives it back; a task the thin CAM was still holding completes now.
//-----------------------------------------------------------------------------

void
SAS2308Glue::HandleTimeout(void *task)
{
    SAS2308GluePort::TaskDesc d;

    if (!Enter()) {
        Klog(MPSK_RECOVERY | MPSK_ERROR, "timeout while stopping: the stop path completes the task");
        return;
    }
    Lock();
    fKStats.familyTimeouts++;
    Unlock();
    const int rc = fPort->TaskDescribe(task, &d) ? mps_kext_timeout(fMps, d.td, task) : -1;
    Leave();
    if (rc != 0)
        Klog(MPSK_RECOVERY | MPSK_ERROR, "timeout for a task that is not outstanding, ignored");
    DeliverCompletions();
}

void *
SAS2308Glue::FindTask(uint32_t target, uint64_t lun, uint64_t tag)
{
    void *victim = nullptr;

    Lock();
    for (uint32_t i = 0; i < fMaxTasks && !victim; i++) {
        const Slot &s = fSlots[i];
        if (s.task && s.target == target && s.lun == lun && s.tag == tag)
            victim = s.task;
    }
    Unlock();
    return victim;
}

//-----------------------------------------------------------------------------
//	DeliverCompletions - mpssas_scsiio_complete() results to the family.
//	No lock held.
//-----------------------------------------------------------------------------

void
SAS2308Glue::DeliverCompletions(void)
{
    if (!fMps || !fLock)
        return;
    for (;;) {
        struct mps_kext_io_result res;
        void *task = mps_kext_next_done(fMps, &res);
        if (!task)
            break;

        bool prepared = false;
        Lock();
        for (uint32_t i = 0; i < fMaxTasks; i++) {
            if (fSlots[i].task == task) {
                prepared = fSlots[i].prepared;
                fSlots[i] = Slot();
                break;
            }
        }
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
             * Reset (CAM_SCSI_BUS_RESET: reinit, or the IOC stopped for
             * sleep), OS abort (CAM_REQ_ABORTED), transport errors: not the
             * device's fault, a failure the upper layers retry.
             */
            done = SAS2308GluePort::kDoneRetry;
            break;
        }
        fPort->CompleteTask(task, done, res.scsi_status, res.transferred, res.sense, res.sense_len);
    }
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
//	reinit if the rediscovery finds them again).
//-----------------------------------------------------------------------------

void
SAS2308Glue::Async(int event, uint64_t tids, uint64_t present, const uint32_t *gens)
{
    if (event == MPSK_ASYNC_BUS_RESET) {
        Klog(MPSK_MAPPING | MPSK_RECOVERY, "IOC reset: targets wait for the rediscovery");
        return;
    }
    if (event == MPSK_ASYNC_SENT_BDR) {
        for (uint32_t i = 0; i < kMaxTargets; i++)
            if (tids & (1ull << i))
                Klog(MPSK_RECOVERY, "target %u: target reset sent", i);
        return;
    }
    if (!fLock)
        return;
    Lock();
    if (fTargets.Update(tids, present, gens))
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
             * (the removal's target reset), deliver, then destroy.
             */
            if (Enter()) {
                const int left = mps_kext_target_drain(fMps, w.targetID, kDrainMs);
                Leave();
                DeliverCompletions();
                if (left)
                    Klog(MPSK_MAPPING | MPSK_ERROR, "target %u: %d IOs still outstanding at its destroy",
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
        DeliverCompletions();
        for (uint32_t id = 0; !present && id < kMaxTargets; id++) {
            if (!Enter())
                break;
            const int left = mps_kext_target_drain(fMps, id, kDrainMs);
            Leave();
            DeliverCompletions();
            if (left)
                Klog(MPSK_MAPPING | MPSK_ERROR, "target %u: %d IOs still outstanding at termination", id, left);
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
//	its loop destroys IDs 0 .. familyLimit - 1 only, so destroy ID 63
//	ourselves (DestroyTargetForID of a missing ID is a no-op).
//-----------------------------------------------------------------------------

void
SAS2308Glue::TerminateTargets(uint32_t familyLimit)
{
    uint64_t left = 0;

    if (fLock) {
        Lock();
        left = fTargets.Leftovers(familyLimit);
        Unlock();
    }
    for (uint32_t i = 0; i < kMaxTargets; i++) {
        if (left & (1ull << i)) {
            fPort->DestroyTarget(i);
            Klog(MPSK_MAPPING | MPSK_INFO, "target %u destroyed (missed by the family's willTerminate)", i);
        }
    }
}
