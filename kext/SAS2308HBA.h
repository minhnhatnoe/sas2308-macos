/*
 * SAS2308HBA.h - IOKit driver for the LSI/Broadcom SAS2308 SAS HBA
 * (SAS 9207-8i, PCI 1000:0087, MPI 2.0 IT firmware).
 *
 * The class is an IOSCSIParallelInterfaceController. The MPI2 protocol is
 * the FreeBSD mps(4) driver (core/), reached only through mps_xnu_os.h
 * (plain C types; mps_xnu_os.cpp has the core's mps_os_* hooks on xnu).
 *
 * Source layout (IntelMausi / itlwm style, one file per concern):
 *   SAS2308HBA.cpp          IOService life cycle, SCSI family callbacks
 *                           (capabilities, interrupts, IO, task management)
 *   SAS2308HBA_PM.cpp       power management, systemWillShutdown
 *   SAS2308HBA_Hotplug.cpp  target creation / destruction, per-target
 *                           properties, termination of the targets
 *   SAS2308HBA_Props.cpp    registry properties: logging control,
 *                           controller characteristics, statistics
 *   SAS2308HBA_Targets.h    the target table (no IOKit, host-tested)
 *   mps_xnu_os.{h,cpp}      the core's platform hooks (mps_pci.c's role)
 */
#ifndef _SAS2308HBA_H
#define _SAS2308HBA_H

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
#include "SAS2308HBA_Targets.h"

class SAS2308HBA : public IOSCSIParallelInterfaceController
{
    OSDeclareDefaultStructors(SAS2308HBA)

public:
    static constexpr uint32_t kMaxTargets = MPSK_MAX_TARGETS;   /* target IDs 0..63 */
    static constexpr uint32_t kMaxTasks = MPSK_MAX_TASKS;       /* ReportMaximumTaskCount */
    static constexpr uint32_t kMaxLun = 255;                    /* peripheral LUN addressing */
    static constexpr uint32_t kStatsPublishMs = 1000;           /* statistics timer */
    static constexpr uint32_t kSleepDrainMs = 10000;            /* system sleep: IO drain bound */
    static constexpr uint32_t kShutdownDrainMs = 2000;          /* restart / power off */

    /* Registry keys (personality default, boot-args, setProperties). */
    static constexpr const char *kLogLevelKey   = "SAS2308LogLevel";    /* 0..4 or error/warn/info/debug/trace */
    static constexpr const char *kDebugMaskKey  = "SAS2308DebugMask";   /* FreeBSD mps_debug mask or class list */
    static constexpr const char *kIOLogKey      = "SAS2308LogToIOLog";  /* bool: IOLog instead of os_log */
    static constexpr const char *kResetStatsKey = "SAS2308ResetStats";
    static constexpr const char *kStatsKey      = "SAS2308Statistics";
    static constexpr const char *kIocTemperatureKey = "IOC Temperature (C)";   /* IO Unit Page 7 */

    /* IOService (start/stop belong to the family: "shall not be overridden") */
    virtual IOReturn    setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) APPLE_KEXT_OVERRIDE;
    virtual void        systemWillShutdown(IOOptionBits specifier) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;

protected:
    /* IOService, as the family allows ("should call the superclass implementation") */
    virtual bool willTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;

    /* IOSCSIParallelInterfaceController: controller life cycle */
    virtual bool InitializeController(void) APPLE_KEXT_OVERRIDE;
    virtual bool StartController(void) APPLE_KEXT_OVERRIDE;
    virtual void StopController(void) APPLE_KEXT_OVERRIDE;
    virtual void TerminateController(void) APPLE_KEXT_OVERRIDE;

    /* capabilities */
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

    /* interrupts */
    virtual IOInterruptEventSource *CreateDeviceInterrupt(
        IOInterruptEventSource::Action action,
        IOFilterInterruptEventSource::Filter filter,
        IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual bool FilterInterruptRequest(void) APPLE_KEXT_OVERRIDE;
    virtual void HandleInterruptRequest(void) APPLE_KEXT_OVERRIDE;

    /* IO */
    virtual SCSIServiceResponse ProcessParallelTask(SCSIParallelTaskIdentifier parallelRequest) APPLE_KEXT_OVERRIDE;
    virtual void HandleTimeout(SCSIParallelTaskIdentifier parallelRequest) APPLE_KEXT_OVERRIDE;

    /* task management (XPT_XNU_TM ccbs: asynchronous in the core, the family API waits) */
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
        uint32_t tasksHeld;             /* ... of which the thin CAM held (recovery, sleep) */
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
    enum { kPowerStateOff = 0, kPowerStateOn, kPowerStateCount };

    /* ---- SAS2308HBA.cpp: core callbacks (mps_mtx may be held: no family or mps_kext_* calls) */
    static int  HostSetupInterrupts(void *hba);
    static void HostFreeInterrupts(void *hba);
    static int  HostPciRestore(void *hba);
    static void HostControllerDead(void *hba);
    static void HostDoneKick(void *hba);

    bool Enter(void);                   /* gate around mps_kext_* calls; false once stopping */
    void Leave(void);
    void StopCore(void);                /* stop new calls, wait for the running ones, mps_free */
    void ReleaseResources(void);        /* idempotent */
    void DeliverCompletions(void);      /* no lock held: CompleteParallelTask for queued ccbs */
    static void DoneThread(thread_call_param_t self, thread_call_param_t);
    void ScheduleDone(void);            /* fLock held */
    void QuiesceDone(void);             /* fLock not held; may sleep */
    SCSIServiceResponse SendTaskMgmt(uint8_t taskType, SCSITargetIdentifier theT,
                                     SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ);

    /* ---- SAS2308HBA_PM.cpp */
    void InitializePowerManagement(IOService *provider);
    void StopPowerManagement(void);
    bool QuiesceController(uint32_t drainMs, const char *why);   /* sleep / shutdown */
    void ResumeController(void);                                  /* wake */

    /* ---- SAS2308HBA_Hotplug.cpp */
    static void HostAsync(void *hba, int event, uint64_t tids, uint64_t present);
    static void HotplugThread(thread_call_param_t self, thread_call_param_t);
    void Hotplug(void);
    void ScheduleHotplug(void);         /* fLock held */
    void QuiesceHotplug(void);          /* fLock not held; may sleep */
    OSDictionary *CopyTargetProperties(SCSITargetIdentifier targetID);
    void PublishTargetCharacteristics(SCSITargetIdentifier targetID);
    void TerminateTargets(void);        /* willTerminate, after the family's loop */

    /* ---- SAS2308HBA_Props.cpp */
    void Klog(uint32_t cls, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
    void ApplyLogSettings(void);
    void PublishControllerCharacteristics(void);
    bool Snapshot(struct mps_kext_snapshot *snap);
    void PublishStats(bool force);
    void LogCounters(const char *why);
    static void StatsTimerFired(OSObject *owner, IOTimerEventSource *sender);
    static void SetNumber(OSDictionary *dict, const char *key, UInt64 value);

    IOPCIDevice   *fPCI   = nullptr;
    IOMemoryMap   *fMap   = nullptr;
    IOMapper      *fMapper = nullptr;   /* the DART in front of fPCI, or NULL */
    IOLock        *fLock  = nullptr;    /* class state below; a leaf under mps_mtx */
    thread_call_t  fHotplugCall = nullptr;
    thread_call_t  fDoneCall = nullptr; /* delivers completions queued by callouts / the reinit task */
    IOTimerEventSource *fStatsTimer = nullptr;
    bool           fBusLeadOff = false;     /* bus mastering cut after the IOC died */
    bool           fStarted = false;
    bool           fStopping = false;       /* no new mps_kext_* calls */
    bool           fCoreAttached = false;   /* mps_attach done, mps_free not yet */
    bool           fHotplugStop = false;    /* no new hotplug calls */
    bool           fDoneStop = false;
    bool           fTunnelled = false;      /* IOPCITunnelled: behind Thunderbolt */
    uint32_t       fHotplugPending = 0;     /* thread calls queued or running */
    uint32_t       fDonePending = 0;
    uint32_t       fBusy = 0;               /* threads inside Enter()/Leave() */
    uint32_t       fMaxSegs = 0;            /* per IO, from the core's buffer_dmat */

    /* power management (SAS2308HBA_PM.cpp) */
    bool           fPMInitialized = false;
    bool           fSuspended = false;      /* IOC stopped for sleep / shutdown */
    bool           fShutdown = false;       /* systemWillShutdown ran: stays stopped */
    unsigned long  fPowerState = kPowerStateOn;

    /* the core */
    struct mps_kext      *fMps = nullptr;
    struct mps_kext_host  fHost = {};

    /* logging and counters */
    uint32_t       fDebugMask = 0;
    uint32_t       fUnit = 0;
    KStats         fKStats = {};
    uint64_t       fLastPublishUs = 0;
    uint64_t       fLastPublishedSig = ~0ull;

    Slot               fSlots[kMaxTasks] = {};
    SAS2308TargetTable fTargets;            /* under fLock */
};

#endif /* _SAS2308HBA_H */
