/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * SAS2308Glue.h - the IOKit-free half of the SAS2308HBA class: the gate
 * around the core, ProcessParallelTask / HandleTimeout /
 * completion delivery, the hotplug thread and its target table.
 *
 * Everything that needs IOKit or the family (task accessors, IODMACommand,
 * SetTimeoutForTask, CompleteParallelTask, Create/DestroyTargetForID, thread
 * calls) goes through SAS2308GluePort, which the class implements
 * (SAS2308HBA.cpp) and test/kext_glue_test.cpp fakes with the family's
 * contract (TF1 of the review): only tasks given to SetTimeoutForTask are
 * timed, a task the HBA accepted is finished only by CompleteParallelTask,
 * no completion after DestroyTargetForID, thread calls with xnu semantics
 * (an enter while the call runs starts a second, concurrent invocation).
 *
 * Locking: fLock (mps_kext_lock: IOLock on xnu) protects every member below;
 * it is a leaf under mps_mtx (the core's callbacks take it). No port call
 * that reaches the family is made with it held.
 */
#ifndef SAS2308_GLUE_H
#define SAS2308_GLUE_H

#include <stddef.h>
#include <stdint.h>

#include "mps_xnu_os.h"
#include "SAS2308HBA_Targets.h"

#define SAS2308_HIDDEN __attribute__((visibility("hidden")))

class SAS2308_HIDDEN SAS2308GluePort
{
public:
    /* how a task finished (the class maps it to SCSITaskStatus / SCSIServiceResponse) */
    enum Done {
        kDoneStatus,        /* the device answered: scsiStatus, transfer count, sense */
        kDoneGone,          /* DeviceNotPresent */
        kDoneTimeout,       /* TaskTimeoutOccurred (the family timer fired, D44) */
        kDoneRetry,         /* DeliveryFailure (reset, abort, transport) */
        kDoneReject         /* FUNCTION_REJECTED (invalid request) */
    };
    struct TaskDesc {
        uint32_t target;
        uint64_t lun;
        uint64_t tag;
        uint8_t  cdb[16];
        uint8_t  cdbLen;
        int      dir;               /* MPSK_DIR_* */
        uint64_t length;
        void    *td;                /* HBA data: the ccb (mps_kext_task_size) */
        size_t   tdSize;
        uint32_t timeoutMs;         /* the task's own timeout, 0 = none */
        int      attr;              /* MPSK_ATTR_* (SCSITaskAttribute) */
    };
    enum { kCallHotplug, kCallDone };

    virtual bool TaskDescribe(void *task, TaskDesc *d) = 0;
    /* IODMACommand::prepare / gen64IOVMSegments / complete */
    virtual bool TaskMapBegin(void *task, uint64_t length) = 0;
    virtual bool TaskMapNext(void *task, uint64_t *offset, struct mps_kext_seg *chunk, uint32_t *n) = 0;
    virtual void TaskUnmap(void *task) = 0;
    virtual void SetTimeoutForTask(void *task, uint32_t ms) = 0;  /* 0: the task's own timeout */
    virtual void CompleteTask(void *task, int done, uint8_t scsiStatus, uint64_t transferred,
                              const uint8_t *sense, uint32_t senseLen) = 0;
    virtual bool CreateTarget(uint32_t id) = 0;     /* false: not created (also when terminating) */
    virtual void DestroyTarget(uint32_t id) = 0;
    virtual bool CallEnter(int which) = 0;          /* thread_call_enter: true = was already queued */
    virtual bool CallCancel(int which) = 0;         /* thread_call_cancel: true = was queued */
    virtual void Log(uint32_t cls, const char *line) = 0;

protected:
    ~SAS2308GluePort() {}
};

class SAS2308_HIDDEN SAS2308Glue
{
public:
    /*
     * A removed target's IO, before its nub goes: after this long without
     * it (MPS_RESET_TIMEOUT 30 s for the removal's target reset, plus as
     * long for its reinit), the drain asks for a reinit and keeps waiting.
     */
    static constexpr uint32_t kDrainReinitMs = 60000;

    static constexpr uint32_t kAllTargets = ~0u;

    enum Response { kInProcess, kRejected, kFailure };
    enum MapResult { kMapOk, kMapShort, kMapBad };
    struct KStats {                     /* glue-level counters (core counters: struct mps_xnu_stats) */
        uint64_t tasks;                 /* ProcessParallelTask accepted */
        uint32_t tasksHeld;             /* ... of which the thin CAM held (recovery, sleep) */
        uint32_t tasksRejected;         /* not accepted (mapping, invalid, gone) */
        uint32_t familyTimeouts;        /* HandleTimeout calls */
        uint32_t tmRequests, tmRequestsFailed;
        uint32_t deadEvents;            /* controller declared dead */
        uint32_t lostKicks;             /* done queue left with no kick for two 1 s ticks (D43) */
        uint32_t tmInGate;              /* OS TMs whose caller held the work-loop gate (D43) */
        uint32_t mapWaits;              /* tasks that waited for a DMA mapping (D47) */
    };

    bool Init(SAS2308GluePort *port);   /* the lock; false: no memory */
    void Fini(void);                    /* idempotent; the lock stays (L7) */
    /*
     * The lock goes last, from free() (L7): by then no userland entry point
     * (setProperties) can run, as FreeBSD's mps_free destroys mps_mtx after
     * draining the ioctl and sysctl handlers. From Init to FreeLock
     * HasLock() is constant, so its unlocked read is benign.
     */
    void FreeLock(void);
    bool HasLock(void) const { return fLock != nullptr; }
    struct mps_kext *TakeCore(void);    /* fMps = nullptr under the lock; returns the old one */
    bool Snapshot(struct mps_kext_snapshot *snap);  /* attached: gated; detached: under the lock */
    void SetDebug(uint32_t mask);       /* the core's mask, under the lock */
    void Lock(void) { mps_kext_lock(fLock); }
    void Unlock(void) { mps_kext_unlock(fLock); }

    bool Enter(void);                   /* gate around mps_kext_* calls; false once stopping */
    void Leave(void);
    bool Attached(void);                /* mps_attach done: fCoreAttached, fMaxSegs, fMaxTasks, fMaxTargets; false: no memory */
    void Start(void);                   /* StartController */
    void StopCore(void);                /* no new calls, wait for running ones, mps_free */

    Response ProcessTask(void *task);
    bool AbortWaiting(void *task);      /* a task waiting for its mapping: completed (D47) */
    void FlushMapWait(uint32_t target, int done);   /* no lock held; may sleep */
    void HandleTimeout(void *task);
    uint32_t DeliverCompletions(void);  /* no lock held; returns how many */
    void TimerDeliver(void);            /* the 1 s statistics timer: detects lost kicks, delivers nothing (D43) */

    void ScheduleDone(void);            /* fLock held */
    void DoneCallBody(void);            /* the done thread call */
    void QuiesceDone(void);             /* fLock not held; may sleep */

    /* CAM async event (mps_mtx held) for `tid' or MPSK_ALL_TARGETS: `present'/`gens' sampled per target id */
    void Async(int event, uint32_t tid, const uint8_t *present, const uint32_t *gens, uint32_t count);
    void ScheduleHotplug(void);         /* fLock held */
    void HotplugCallBody(void);         /* the hotplug thread call */
    void QuiesceHotplug(void);          /* fLock not held; may sleep */
    bool WillTerminate(void);           /* before the family's willTerminate; false: controller gone */
    void StopTargets(void);             /* willTerminate: no new hotplug work */
    void TerminateTargets(uint32_t familyLimit);   /* after the family's willTerminate */

    void Klog(uint32_t cls, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

    /* state: read by the class (PM, properties) with fLock held */
    SAS2308GluePort   *fPort = nullptr;
    void              *fLock = nullptr;
    struct mps_kext   *fMps = nullptr;
    uint32_t           fMaxSegs = 0;            /* per IO, from the core's buffer_dmat */
    uint32_t           fMaxTasks = 0;           /* ReportMaximumTaskCount, from IOCFacts at attach */
    uint32_t           fMaxTargets = 0;         /* target IDs 0 .. fMaxTargets-1, from IOCFacts at attach (D7) */
    uint32_t           fOutstanding = 0;        /* accepted tasks not completed to the family yet */
    bool               fStarted = false;
    bool               fStopping = false;       /* no new mps_kext_* calls */
    bool               fCoreAttached = false;   /* mps_attach done, mps_free not yet */
    bool               fHotplugStop = false;    /* no new hotplug calls */
    bool               fDoneStop = false;
    bool               fHotplugActive = false;  /* an invocation runs the loop */
    bool               fHotplugAgain = false;   /* ... and must scan again before it leaves */
    uint32_t           fHotplugPending = 0;     /* thread call invocations queued or running */
    uint32_t           fDonePending = 0;
    uint32_t           fLostTicks = 0;      /* 1 s ticks with completions and no kick (TimerDeliver, D43) */
    uint32_t           fBusy = 0;               /* threads inside Enter()/Leave() */
    KStats             fKStats = {};
    SAS2308TargetTable fTargets;

private:
    void Hotplug(void);
    MapResult MapTask(void *task, const SAS2308GluePort::TaskDesc &d, uint32_t *nsegs);
    void Post(void *task, const SAS2308GluePort::TaskDesc &d, uint32_t nsegs);
    void RetryMapWait(void);            /* no lock held */
    bool TakeWaiting(void *td, int done);   /* fLock held */

    void              *fMapWaitHead = nullptr;  /* HBA data of tasks waiting for a mapping, in order (D47) */
    void              *fMapWaitTail = nullptr;
    bool               fMapRetrying = false;
};

#endif /* SAS2308_GLUE_H */
