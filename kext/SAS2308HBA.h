/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
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
 *   SAS2308Glue.{h,cpp}     the IOKit-free half of the class: task slots,
 *                           ProcessParallelTask / HandleTimeout / delivery,
 *                           the hotplug thread (host-tested against a fake
 *                           family, test/kext_glue_test.cpp)
 *   mps_kext.cpp            the mps_kext_*() calls (portable, host-tested)
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
#include "SAS2308Glue.h"

class SAS2308HBA;

/* SAS2308GluePort on IOKit: forwards to SAS2308HBA's Port* methods. */
class SAS2308_HIDDEN SAS2308HBAPort : public SAS2308GluePort
{
public:
    SAS2308HBA *fHba = nullptr;
    bool TaskDescribe(void *task, TaskDesc *d) override;
    bool TaskMapBegin(void *task, uint64_t length) override;
    bool TaskMapNext(void *task, uint64_t *offset, struct mps_kext_seg *chunk, uint32_t *n) override;
    void TaskUnmap(void *task) override;
    void SetTimeoutForTask(void *task, uint32_t ms) override;
    void CompleteTask(void *task, int done, uint8_t scsiStatus, uint64_t transferred,
                      const uint8_t *sense, uint32_t senseLen) override;
    bool CreateTarget(uint32_t id) override;
    void DestroyTarget(uint32_t id) override;
    bool CallEnter(int which) override;
    bool CallCancel(int which) override;
    void Log(uint32_t cls, const char *line) override;
};

class SAS2308HBA : public IOSCSIParallelInterfaceController
{
    friend class SAS2308HBAPort;
    OSDeclareDefaultStructors(SAS2308HBA)

public:
    static constexpr uint32_t kMaxLun = 255;                    /* peripheral LUN addressing */
    static constexpr uint32_t kStatsPublishMs = 1000;           /* statistics timer */
    static constexpr uint32_t kSleepDrainMs = 10000;            /* system sleep: IO drain bound */
    static constexpr uint32_t kShutdownDrainMs = 1000;          /* restart / power off (core caps it at 1 s) */

    /* Registry keys (personality default, boot-args, setProperties). */
    static constexpr const char *kLogLevelKey   = "SAS2308LogLevel";    /* 0..4 or error/warn/info/debug/trace */
    static constexpr const char *kDebugMaskKey  = "SAS2308DebugMask";   /* FreeBSD mps_debug mask or class list */
    static constexpr const char *kIOLogKey      = "SAS2308LogToIOLog";  /* bool: IOLog instead of os_log */
    static constexpr const char *kResetStatsKey = "SAS2308ResetStats";
    static constexpr const char *kStatsKey      = "SAS2308Statistics";
    static constexpr const char *kIocTemperatureKey = "IOC Temperature (C)";   /* IO Unit Page 7 */
    static constexpr const char *kLimitsKey     = "Controller Limits";  /* IOCFacts + queue sizes */
    static constexpr const char *kPcieLinkKey   = "PCIe Link";          /* the card's link, MPS, MRRS */

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

    /* task management (D10: XPT_RESET_DEV / the timeout recovery; the family API waits) */
    virtual SCSIServiceResponse AbortTaskRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL,
                                                 SCSITaggedTaskIdentifier theQ) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse AbortTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse ClearACARequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse ClearTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse LogicalUnitResetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) APPLE_KEXT_OVERRIDE;
    virtual SCSIServiceResponse TargetResetRequest(SCSITargetIdentifier theT) APPLE_KEXT_OVERRIDE;

private:
    typedef SAS2308Glue::KStats KStats;
    enum { kPowerStateOff = 0, kPowerStateOn, kPowerStateCount };

    /* ---- SAS2308HBA.cpp: core callbacks (mps_mtx may be held: no family or mps_kext_* calls) */
    static int  HostSetupInterrupts(void *hba);
    static void HostFreeInterrupts(void *hba);
    static int  HostPciRestore(void *hba);
    static void HostControllerDead(void *hba);
    static int HostPciPresent(void *hba);
    static int HostGetenv(void *hba, const char *name, char *buf, size_t len);
    static void HostDoneKick(void *hba);

    /* the gate and delivery live in fGlue (SAS2308Glue.cpp) */
    bool Enter(void) { return fGlue.Enter(); }
    void Leave(void) { fGlue.Leave(); }
    void DeliverCompletions(void) { (void)fGlue.DeliverCompletions(); }
    void ReleaseResources(void);        /* idempotent */
    static void DoneThread(thread_call_param_t self, thread_call_param_t);
    SCSIServiceResponse SendTaskMgmt(uint8_t taskType, SCSITargetIdentifier theT,
                                     SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ);

    /* ---- SAS2308HBA_PM.cpp */
    void InitializePowerManagement(IOService *provider);
    void StopPowerManagement(void);
    bool QuiesceController(uint32_t drainMs, const char *why, bool shutdown);   /* sleep / shutdown */
    void ResumeController(void);                                  /* wake */

    /* ---- SAS2308HBA_Hotplug.cpp */
    static void HostAsync(void *hba, int event, uint32_t tid, const uint8_t *present, const uint32_t *gens,
                          uint32_t count);
    static void HotplugThread(thread_call_param_t self, thread_call_param_t);
    OSDictionary *CopyTargetProperties(SCSITargetIdentifier targetID);
    void PublishTargetCharacteristics(SCSITargetIdentifier targetID);
    /* SAS2308GluePort on IOKit (SAS2308HBA.cpp, SAS2308HBA_Hotplug.cpp) */
    bool PortTaskDescribe(SCSIParallelTaskIdentifier task, SAS2308GluePort::TaskDesc *d);
    bool PortTaskMapBegin(SCSIParallelTaskIdentifier task, uint64_t length);
    bool PortTaskMapNext(SCSIParallelTaskIdentifier task, uint64_t *offset, struct mps_kext_seg *chunk, uint32_t *n);
    void PortTaskUnmap(SCSIParallelTaskIdentifier task);
    void PortCompleteTask(SCSIParallelTaskIdentifier task, int done, uint8_t scsiStatus, uint64_t transferred,
                          const uint8_t *sense, uint32_t senseLen);
    bool PortCreateTarget(uint32_t id);

    /* ---- SAS2308HBA_Props.cpp */
    void Klog(uint32_t cls, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
    void KlogAlways(uint32_t cls, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
    void ApplyLogSettings(void);
    void PublishControllerCharacteristics(void);
    bool Snapshot(struct mps_kext_snapshot *snap);
    void PublishStats(bool force);
    void ReadMsi(uint16_t *command, uint16_t *control, uint64_t *address, uint32_t *data);
    void PublishLimits(bool log);
    void PublishPcieLink(bool log);
    void LogCounters(const char *why);
    static void StatsTimerFired(OSObject *owner, IOTimerEventSource *sender);
    static void SetNumber(OSDictionary *dict, const char *key, UInt64 value);

    IOPCIDevice   *fPCI   = nullptr;
    IOMemoryMap   *fMap   = nullptr;
    IOMapper      *fMapper = nullptr;   /* the DART in front of fPCI, or NULL */
    thread_call_t  fHotplugCall = nullptr;
    thread_call_t  fDoneCall = nullptr; /* delivers completions queued by callouts / the reinit task */
    IOTimerEventSource *fStatsTimer = nullptr;
    bool           fBusLeadOff = false;     /* bus mastering cut after the IOC died */
    bool           fPciSaved = false;       /* saveDeviceState done: diag resets restore it (D13) */
    uint32_t       fPciRestores = 0;        /* restoreDeviceState calls, and failures */
    uint32_t       fPciRestoreFailures = 0;
    bool           fTunnelled = false;      /* IOPCITunnelled: behind Thunderbolt */
    bool           fPostedWrite = false;    /* BAR1 mapped kIOMapPostedWrite (sas2308_posted_write, D75) */

    /* power management (SAS2308HBA_PM.cpp) */
    bool           fPMInitialized = false;
    bool           fSuspended = false;      /* IOC stopped for sleep / shutdown */
    bool           fShutdown = false;       /* systemWillShutdown ran: stays stopped */
    unsigned long  fPowerState = kPowerStateOn;

    /* the core (fGlue.fMps) */
    struct mps_kext_host  fHost = {};

    /* logging and counters */
    uint32_t       fDebugMask = 0;
    uint32_t       fUnit = 0;
    uint64_t       fLastPublishUs = 0;
    uint64_t       fLastPublishedSig = ~0ull;
    uint64_t       fLimitsSig = ~0ull;      /* reinits + resumes when "Controller Limits" was published */

    /*
     * Task slots, the gate, the target table, fLock and the glue counters:
     * fGlue's members, under fGlue.Lock() (a leaf under mps_mtx).
     */
    SAS2308Glue        fGlue;
    SAS2308HBAPort     fGluePort;
};

#endif /* _SAS2308HBA_H */
