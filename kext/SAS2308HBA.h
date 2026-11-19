/*
 * SAS2308HBA - IOKit kernel extension for the LSI/Broadcom SAS2308
 * (SAS 9207-8i, PCI 1000:0087, MPI2 IT firmware).
 *
 * This file is only the IOKit glue: PCI resources, interrupt wiring, data
 * buffer mapping and the translation between SCSIParallelTask and the
 * FreeBSD-shaped core (core/mps*.c), which it reaches through
 * mps_xnu_os.{h,cpp} (ccbs, the mps_os_* hooks). All MPI2 protocol logic
 * is in the core.
 */
#ifndef SAS2308HBA_H
#define SAS2308HBA_H

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/scsi/spi/IOSCSIParallelInterfaceController.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <os/log.h>

#include "mps_xnu_os.h"

class SAS2308HBA : public IOSCSIParallelInterfaceController
{
    OSDeclareDefaultStructors(SAS2308HBA)

public:
    static constexpr uint32_t kMaxTargets = MPSK_MAX_TARGETS;   /* target IDs 0..63 */
    static constexpr uint32_t kMaxTasks = MPSK_MAX_TASKS;       /* ReportMaximumTaskCount */
    static constexpr uint32_t kMaxLun = 255;                    /* peripheral LUN addressing */
    static constexpr uint32_t kWatchdogMs = 1000;               /* statistics publication */
    static constexpr uint32_t kStatsPublishMs = 1000;

    /* Registry keys (personality default, boot-args, setProperties). */
    static constexpr const char *kLogLevelKey  = "SAS2308LogLevel";    /* 0..4 or error/warn/info/debug/trace */
    static constexpr const char *kDebugMaskKey = "SAS2308DebugMask";   /* FreeBSD mps_debug mask or class list */
    static constexpr const char *kIOLogKey     = "SAS2308LogToIOLog";  /* bool: IOLog instead of os_log */
    static constexpr const char *kResetStatsKey = "SAS2308ResetStats";
    static constexpr const char *kStatsKey     = "SAS2308Statistics";

    /* Runtime control (sas2308ctl / IORegistryEntrySetCFProperties). Admin only. */
    virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;

protected:
    /* ---- controller life cycle ---- */
    virtual bool InitializeController(void) APPLE_KEXT_OVERRIDE;
    virtual bool StartController(void) APPLE_KEXT_OVERRIDE;
    virtual void StopController(void) APPLE_KEXT_OVERRIDE;
    virtual void TerminateController(void) APPLE_KEXT_OVERRIDE;

    /* ---- capabilities ---- */
    virtual SCSIInitiatorIdentifier ReportInitiatorIdentifier(void) APPLE_KEXT_OVERRIDE;
    virtual SCSIDeviceIdentifier ReportHighestSupportedDeviceID(void) APPLE_KEXT_OVERRIDE;
    virtual SCSILogicalUnitNumber ReportHBAHighestLogicalUnitNumber(void) APPLE_KEXT_OVERRIDE;
    virtual UInt32 ReportMaximumTaskCount(void) APPLE_KEXT_OVERRIDE;
    virtual UInt32 ReportHBASpecificTaskDataSize(void) APPLE_KEXT_OVERRIDE;
    virtual UInt32 ReportHBASpecificDeviceDataSize(void) APPLE_KEXT_OVERRIDE;
    virtual bool DoesHBAPerformDeviceManagement(void) APPLE_KEXT_OVERRIDE;
    virtual bool DoesHBAPerformAutoSense(void) APPLE_KEXT_OVERRIDE;
    virtual bool DoesHBASupportSCSIParallelFeature(SCSIParallelFeature theFeature) APPLE_KEXT_OVERRIDE;
    virtual void ReportHBAConstraints(OSDictionary *constraints) APPLE_KEXT_OVERRIDE;
    virtual bool InitializeDMASpecification(IODMACommand *command) APPLE_KEXT_OVERRIDE;
    virtual bool InitializeTargetForID(SCSITargetIdentifier targetID) APPLE_KEXT_OVERRIDE;

    /* ---- interrupts ---- */
    virtual IOInterruptEventSource *CreateDeviceInterrupt(
        IOInterruptEventSource::Action action,
        IOFilterInterruptEventSource::Filter filter,
        IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual bool FilterInterruptRequest(void) APPLE_KEXT_OVERRIDE;
    virtual void HandleInterruptRequest(void) APPLE_KEXT_OVERRIDE;

    /* ---- IO ---- */
    virtual SCSIServiceResponse ProcessParallelTask(SCSIParallelTaskIdentifier parallelRequest) APPLE_KEXT_OVERRIDE;
    virtual void HandleTimeout(SCSIParallelTaskIdentifier parallelRequest) APPLE_KEXT_OVERRIDE;

    /* ---- task management (XPT_XNU_TM ccbs; asynchronous in the core, the family API waits) ---- */
    virtual SCSIServiceResponse AbortTaskRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL,
                                                 SCSITaggedTaskIdentifier theQ) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse AbortTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse ClearACARequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse ClearTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse LogicalUnitResetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse TargetResetRequest(SCSITargetIdentifier theT) APPLE_KEXT_OVERRIDE;

private:
    struct KStats {                     /* glue-level counters (core counters: struct mps_xnu_stats) */
        uint64_t tasks;                 /* ProcessParallelTask accepted */
        uint32_t tasksHeld;             /* ... of which the thin CAM held (recovery) */
        uint32_t tasksRejected;         /* not accepted (mapping, invalid, gone) */
        uint32_t familyTimeouts;        /* HandleTimeout calls */
        uint32_t tmRequests, tmRequestsFailed;
        uint32_t deadEvents;            /* controller declared dead */
    };
    struct Slot {                       /* an outstanding task, for AbortTaskRequest() */
        SCSIParallelTaskIdentifier task;
        bool        prepared;           /* IODMACommand::prepare() done */
        uint32_t    target;             /* I_T_L_Q nexus */
        uint64_t    lun;
        SCSITaggedTaskIdentifier tag;
    };
    struct Target {
        bool     created;               /* CreateTargetForID() done */
        bool     wantCreate, wantDestroy;
    };

    /* mps_kext_host callbacks (mps_mtx may be held: no family calls, no mps_kext_* calls) */
    static int  HostSetupInterrupts(void *hba);
    static void HostFreeInterrupts(void *hba);
    static int  HostPciRestore(void *hba);
    static void HostControllerDead(void *hba);
    static void HostDoneKick(void *hba);
    static void HostAsync(void *hba, int event, uint64_t tids, uint64_t present);

    bool Enter();                       /* gate around mps_kext_* calls; false once stopping */
    void Leave();
    void StopCore();                    /* stop new calls, wait for the running ones, mps_free */
    void ReleaseResources();
    void DeliverCompletions();          /* no lock held: CompleteParallelTask for queued ccbs */
    static void DoneThread(thread_call_param_t self, thread_call_param_t);
    void ScheduleDone();                /* fLock held */
    void QuiesceDone();                 /* fLock not held; may sleep */
    void ApplyLogSettings();
    void Klog(uint32_t cls, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
    bool Snapshot(struct mps_kext_snapshot *snap);
    void PublishStats(bool force);
    void LogCounters(const char *why);
    SCSIServiceResponse SendTaskMgmt(uint8_t taskType, SCSITargetIdentifier theT,
                                     SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ);
    static void WatchdogFired(OSObject *owner, IOTimerEventSource *sender);
    static void HotplugThread(thread_call_param_t self, thread_call_param_t);
    void Hotplug();
    void ScheduleHotplug();             /* fLock held */
    void QuiesceHotplug();              /* fLock not held; may sleep */

    IOPCIDevice   *fPCI   = nullptr;
    IOMemoryMap   *fMap   = nullptr;
    IOMapper      *fMapper = nullptr;   /* the DART in front of fPCI, or NULL */
    IOLock        *fLock  = nullptr;    /* class state below; a leaf under mps_mtx */
    thread_call_t  fHotplugCall = nullptr;
    thread_call_t  fDoneCall = nullptr; /* delivers completions queued by callouts / the reinit task */
    IOTimerEventSource *fWatchdog = nullptr;
    bool           fBusLeadOff = false;     /* bus mastering cut after the IOC died */
    bool           fStarted = false;
    bool           fStopping = false;       /* no new mps_kext_* calls */
    bool           fCoreAttached = false;   /* mps_attach done, mps_free not yet */
    bool           fHotplugStop = false;    /* no new hotplug calls */
    bool           fDoneStop = false;
    uint32_t       fHotplugPending = 0;     /* thread calls queued or running */
    uint32_t       fDonePending = 0;
    uint32_t       fBusy = 0;               /* threads inside Enter()/Leave() */
    uint32_t       fMaxSegs = 0;            /* per IO, from the core's buffer_dmat */

    /* the core */
    struct mps_kext      *fMps = nullptr;
    struct mps_kext_host  fHost = {};

    /* logging and counters */
    uint32_t       fDebugMask = 0;
    uint32_t       fUnit = 0;
    KStats         fKStats = {};
    uint64_t       fLastPublishUs = 0;
    uint64_t       fLastPublishedIos = ~0ull;

    Slot    fSlots[kMaxTasks] = {};
    Target  fTargets[kMaxTargets] = {};
};

#endif /* SAS2308HBA_H */
