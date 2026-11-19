/*
 * SAS2308HBA.cpp - IOKit glue for the SAS2308 (see SAS2308HBA.h).
 *
 * Status: compiles and links against the macOS 27 Kernel framework; never
 * loaded or run against hardware. Known gaps are listed in design.md.
 *
 * The core is FreeBSD's sys/dev/mps (core/), reached through
 * mps_xnu_os.{h,cpp}: SCSI tasks become XPT_SCSI_IO ccbs, task-management
 * requests XPT_XNU_TM ccbs, the reinit runs on the core's taskqueue and the
 * timers (mps_periodic, TM and IO timeouts) are the core's callouts.
 *
 * Locking (design.md "Locking rules"):
 *  - mps_mtx (an IOLock, inside the core) protects the core. Every
 *    mps_kext_*() call takes it itself; ccb completions (cbfcnp) and CAM
 *    async callbacks run with it held, so they only queue work: completed
 *    tasks go on a done queue that DeliverCompletions() empties without any
 *    lock, from the thread that made the core call or from fDoneCall.
 *  - fLock (an IOLock) protects this class's tables and flags. It is a leaf:
 *    taken inside mps_mtx by the host callbacks, never held across an
 *    mps_kext_*() call or a family call.
 *  - Nothing that calls back into the SCSI family (CompleteParallelTask,
 *    CreateTargetForID, DestroyTargetForID) runs with either lock held.
 *  - Enter()/Leave() count the threads inside the core; StopController sets
 *    fStopping, wakes the TM waiters and waits for the count to drop to 0
 *    before mps_free().
 */
#include "SAS2308HBA.h"

#include <IOKit/IOKitKeys.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <IOKit/scsi/SCSITask.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

/* MPI2_SCSITASKMGMT_TASKTYPE_* (core/mpi/mpi2_init.h) */
enum : uint8_t {
    kTmAbortTask    = 0x01,
    kTmAbortTaskSet = 0x02,
    kTmTargetReset  = 0x03,
    kTmLunReset     = 0x05,
    kTmClearTaskSet = 0x06,
    kTmClearAca     = 0x08,
};

static void SetNumber(OSDictionary *dict, const char *key, UInt64 value);
static volatile SInt32 gUnits = 0;

#define super IOSCSIParallelInterfaceController
OSDefineMetaClassAndStructors(SAS2308HBA, IOSCSIParallelInterfaceController)

static uint64_t UptimeUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

/* ======================================================================
 * Logging: lines go through mps_kext_log() (os_log per class, or IOLog).
 * ==================================================================== */

void SAS2308HBA::Klog(uint32_t cls, const char *fmt, ...)
{
    if (!(cls & fDebugMask))
        return;
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    mps_kext_log(&fHost, cls, line);
}

/*
 * Level: personality property SAS2308LogLevel (Info.plist, default 2 =
 * info), overridden by boot-arg sas2308_log=N (0..4); class mask
 * (FreeBSD hw.mps.N.debug_level): property SAS2308DebugMask or boot-arg
 * sas2308_debug=0xMASK, which replaces the level's mask. Boot-arg
 * sas2308_iolog=1 sends lines to IOLog (dmesg / panic log) instead of os_log.
 */
void SAS2308HBA::ApplyLogSettings()
{
    int level = MPSK_LEVEL_INFO;
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty(kLogLevelKey)))
        level = (int)n->unsigned32BitValue();
    else if (OSString *s = OSDynamicCast(OSString, getProperty(kLogLevelKey)))
        level = mps_kext_parse_level(s->getCStringNoCopy());
    uint32_t v = 0;
    if (PE_parse_boot_argn("sas2308_log", &v, sizeof(v)) && v <= (uint32_t)MPSK_LEVEL_TRACE)
        level = (int)v;
    if (level < 0 || level > MPSK_LEVEL_TRACE)
        level = MPSK_LEVEL_INFO;
    uint32_t mask = mps_kext_level_mask(level);
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty(kDebugMaskKey)))
        mask = n->unsigned32BitValue() & MPSK_DEBUG_ALL;
    else if (OSString *s = OSDynamicCast(OSString, getProperty(kDebugMaskKey)))
        mps_kext_parse_debug(s->getCStringNoCopy(), &mask);
    if (PE_parse_boot_argn("sas2308_debug", &v, sizeof(v)))
        mask = v & MPSK_DEBUG_ALL;
    if (PE_parse_boot_argn("sas2308_iolog", &v, sizeof(v)))
        fHost.use_iolog = v != 0;
    else if (OSBoolean *b = OSDynamicCast(OSBoolean, getProperty(kIOLogKey)))
        fHost.use_iolog = b->isTrue();
    fDebugMask = mask;
    if (fMps)
        mps_kext_set_debug(fMps, mask);
    setProperty(kLogLevelKey, (unsigned long long)level, 32);
    setProperty(kDebugMaskKey, (unsigned long long)mask, 32);
    char names[128];
    mps_kext_describe_debug(mask, names, sizeof(names));
    Klog(MPSK_INIT | MPSK_INFO, "log level %d (%s), debug mask 0x%x (%s), sink %s", level,
         mps_kext_level_name(level), mask, names, fHost.use_iolog ? "IOLog" : "os_log subsystem org.sas2308.driver");
}

/* sas2308ctl / IORegistryEntrySetCFProperties(): change the level at runtime. */
IOReturn SAS2308HBA::setProperties(OSObject *properties)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d)
        return kIOReturnBadArgument;
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;

    bool handled = false;
    uint32_t mask = fDebugMask;
    int level = -1;
    if (OSObject *o = d->getObject(kLogLevelKey)) {
        if (OSNumber *n = OSDynamicCast(OSNumber, o)) level = (int)n->unsigned32BitValue();
        else if (OSString *s = OSDynamicCast(OSString, o)) level = mps_kext_parse_level(s->getCStringNoCopy());
        if (level < 0 || level > MPSK_LEVEL_TRACE)
            return kIOReturnBadArgument;
        mask = mps_kext_level_mask(level);
        handled = true;
    }
    if (OSObject *o = d->getObject(kDebugMaskKey)) {
        if (OSNumber *n = OSDynamicCast(OSNumber, o))
            mask = n->unsigned32BitValue() & MPSK_DEBUG_ALL;
        else if (OSString *s = OSDynamicCast(OSString, o))
            mps_kext_parse_debug(s->getCStringNoCopy(), &mask);   /* FreeBSD mps_parse_debug */
        else
            return kIOReturnBadArgument;
        handled = true;
    }
    if (handled) {
        const uint32_t old = fDebugMask;
        fDebugMask = mask;
        if (fMps)
            mps_kext_set_debug(fMps, mask);
        if (level >= 0)
            setProperty(kLogLevelKey, (unsigned long long)level, 32);
        setProperty(kDebugMaskKey, (unsigned long long)mask, 32);
        char names[128];
        mps_kext_describe_debug(mask, names, sizeof(names));
        /* Logged at error class so the change itself is visible at every level. */
        Klog(MPSK_ERROR | MPSK_INFO, "debug mask 0x%x -> 0x%x (%s), set from userland", old, mask, names);
    }
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject(kIOLogKey))) {
        fHost.use_iolog = b->isTrue();
        setProperty(kIOLogKey, b->isTrue());
        handled = true;
    }
    if (d->getObject(kResetStatsKey)) {
        if (Enter()) {
            mps_kext_reset_stats(fMps);
            Leave();
        }
        if (fLock) {
            IOLockLock(fLock);
            fKStats = KStats();
            IOLockUnlock(fLock);
        }
        Klog(MPSK_INFO, "statistics reset from userland");
        PublishStats(true);
        handled = true;
    }
    return handled ? kIOReturnSuccess : kIOReturnUnsupported;
}

/* ======================================================================
 * Host callbacks from the core (mps_mtx may be held)
 * ==================================================================== */

/* mps_pci_setup_interrupts(): the family owns the event source (CreateDeviceInterrupt). */
int SAS2308HBA::HostSetupInterrupts(void *hba)
{
    (void)hba;
    return 0;
}

void SAS2308HBA::HostFreeInterrupts(void *hba)
{
    (void)hba;                                  /* DisableInterrupt() in StopController */
}

/* mps_pci_restore() after a diag reset: memory decode + bus mastering (MSI). */
int SAS2308HBA::HostPciRestore(void *hba)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    if (self->fPCI && !self->fBusLeadOff) {
        self->fPCI->setMemoryEnable(true);
        self->fPCI->setBusLeadEnable(true);
    }
    return 0;
}

/*
 * mps_xnu_disable(): the controller is gone (surprise removal) or disabled
 * (reinit failed); the core fails everything it held. Cut bus mastering so
 * a wedged IOC cannot DMA any more.
 */
void SAS2308HBA::HostControllerDead(void *hba)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    IOLockLock(self->fLock);
    const bool first = !self->fBusLeadOff;
    self->fKStats.deadEvents++;
    self->fBusLeadOff = true;
    IOLockUnlock(self->fLock);
    if (first) {
        self->Klog(MPSK_FAULT | MPSK_INFO, "controller is gone or disabled: disabling bus mastering");
        if (self->fPCI)
            self->fPCI->setBusLeadEnable(false);
    }
}

/* Completions queued from a callout or the reinit task: deliver them on fDoneCall. */
void SAS2308HBA::HostDoneKick(void *hba)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    IOLockLock(self->fLock);
    self->ScheduleDone();
    IOLockUnlock(self->fLock);
}

/*
 * CAM async events. `present' was sampled (mps_xnu_target_present) when the
 * event happened; the hotplug thread acts on it later. A rescan finds a
 * target present -> create it; absent (removed, or not rediscovered after a
 * reinit: the core rescans the whole bus when port enable completes) ->
 * destroy it. AC_BUS_RESET alone changes nothing (targets survive a reinit
 * if the rediscovery finds them again, as in M4).
 */
void SAS2308HBA::HostAsync(void *hba, int event, uint64_t tids, uint64_t present)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    if (event == MPSK_ASYNC_BUS_RESET) {
        self->Klog(MPSK_MAPPING | MPSK_RECOVERY, "IOC reset: targets wait for the rediscovery");
        return;
    }
    if (event == MPSK_ASYNC_SENT_BDR) {
        for (uint32_t i = 0; i < kMaxTargets; i++)
            if (tids & (1ull << i))
                self->Klog(MPSK_RECOVERY, "target %u: target reset sent", i);
        return;
    }
    bool changed = false;
    IOLockLock(self->fLock);
    for (uint32_t i = 0; i < kMaxTargets; i++) {
        if (!(tids & (1ull << i)))
            continue;
        Target &t = self->fTargets[i];
        if (present & (1ull << i)) {
            if ((!t.created && !t.wantCreate) || t.wantDestroy) {
                self->Klog(MPSK_MAPPING | MPSK_INFO, "target %u present: %s", i,
                           t.created ? "keeping it" : "creating it");
                t.wantCreate = !t.created;
                t.wantDestroy = false;
                changed = true;
            }
        } else if (t.created || t.wantCreate) {
            if (!t.wantDestroy || t.wantCreate)
                self->Klog(MPSK_MAPPING | MPSK_INFO, "target %u not present: removing it", i);
            t.wantDestroy = t.created;
            t.wantCreate = false;
            changed = true;
        }
    }
    if (changed)
        self->ScheduleHotplug();
    IOLockUnlock(self->fLock);
}

/* ======================================================================
 * The gate around the core, completion delivery
 * ==================================================================== */

bool SAS2308HBA::Enter()
{
    if (!fLock)
        return false;
    IOLockLock(fLock);
    const bool ok = !fStopping && fCoreAttached && fMps;
    if (ok)
        fBusy++;
    IOLockUnlock(fLock);
    return ok;
}

void SAS2308HBA::Leave()
{
    IOLockLock(fLock);
    if (--fBusy == 0 && fStopping)
        IOLockWakeup(fLock, &fBusy, false);
    IOLockUnlock(fLock);
}

/* mpssas_scsiio_complete() result -> SAM status for the family. No lock held. */
void SAS2308HBA::DeliverCompletions()
{
    if (!fMps)
        return;
    for (;;) {
        struct mps_kext_io_result res;
        SCSIParallelTaskIdentifier task = (SCSIParallelTaskIdentifier)mps_kext_next_done(fMps, &res);
        if (!task)
            break;

        bool prepared = false;
        IOLockLock(fLock);
        for (uint32_t i = 0; i < kMaxTasks; i++) {
            if (fSlots[i].task == task) {
                prepared = fSlots[i].prepared;
                fSlots[i] = Slot();
                break;
            }
        }
        IOLockUnlock(fLock);
        if (prepared) {
            IODMACommand *dma = GetDMACommand(task);
            if (dma)
                dma->complete();
        }

        SCSITaskStatus status;
        SCSIServiceResponse response = kSCSIServiceResponse_TASK_COMPLETE;
        switch (res.kind) {
        case MPSK_IO_STATUS:
            /* CAM_REQ_CMP / CAM_SCSI_STATUS_ERROR: the SAM status byte, same encoding as SCSITaskStatus */
            status = (SCSITaskStatus)res.scsi_status;
            SetRealizedDataTransferCount(task, res.transferred);
            if (res.sense_len)
                SetAutoSenseData(task, (SCSI_Sense_Data *)res.sense, (UInt8)res.sense_len);
            break;
        case MPSK_IO_GONE:
            status = kSCSITaskStatus_DeviceNotPresent;
            response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            break;
        case MPSK_IO_TIMEOUT:
            /* terminated by the recovery after the family's timeout (CAM_CMD_TIMEOUT) */
            status = kSCSITaskStatus_ProtocolTimeoutOccurred;
            response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            break;
        default:
            /*
             * Reset (CAM_SCSI_BUS_RESET), OS abort (CAM_REQ_ABORTED), transport
             * errors: not the device's fault, a failure the upper layers retry.
             * The core logged the decoded failure (rate-limited).
             */
            status = kSCSITaskStatus_DeliveryFailure;
            response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            break;
        }
        CompleteParallelTask(task, status, response);
    }
}

void SAS2308HBA::DoneThread(thread_call_param_t self, thread_call_param_t)
{
    SAS2308HBA *hba = (SAS2308HBA *)self;

    hba->DeliverCompletions();
    IOLockLock(hba->fLock);
    if (--hba->fDonePending == 0)
        IOLockWakeup(hba->fLock, &hba->fDonePending, false);
    IOLockUnlock(hba->fLock);
}

/*
 * thread_call_cancel_wait() is only exported by com.apple.kpi.private,
 * which a third-party kext cannot link against. So count the invocations
 * that are queued or running ourselves; Quiesce*() waits for zero.
 * thread_call_enter() returns TRUE if the call was already queued, in
 * which case no additional invocation will happen.
 */
void SAS2308HBA::ScheduleDone()
{
    if (!fDoneCall || fDoneStop)
        return;
    if (!thread_call_enter(fDoneCall))
        fDonePending++;
}

void SAS2308HBA::QuiesceDone()
{
    if (!fDoneCall || !fLock)
        return;
    IOLockLock(fLock);
    fDoneStop = true;
    if (thread_call_cancel(fDoneCall) && fDonePending)
        fDonePending--;                             /* was queued, will not run */
    while (fDonePending)
        IOLockSleep(fLock, &fDonePending, THREAD_UNINT);
    IOLockUnlock(fLock);
}

/* ======================================================================
 * Target creation runs on its own thread: CreateTargetForID() probes the
 * device synchronously, and those probe IOs complete on the work loop.
 * ==================================================================== */

void SAS2308HBA::HotplugThread(thread_call_param_t self, thread_call_param_t)
{
    SAS2308HBA *hba = (SAS2308HBA *)self;

    hba->Hotplug();
    IOLockLock(hba->fLock);
    if (--hba->fHotplugPending == 0)
        IOLockWakeup(hba->fLock, &hba->fHotplugPending, false);
    IOLockUnlock(hba->fLock);
}

void SAS2308HBA::ScheduleHotplug()
{
    if (!fHotplugCall || fHotplugStop)
        return;
    if (!thread_call_enter(fHotplugCall))
        fHotplugPending++;
}

void SAS2308HBA::QuiesceHotplug()
{
    if (!fHotplugCall || !fLock)
        return;
    IOLockLock(fLock);
    fHotplugStop = true;
    if (thread_call_cancel(fHotplugCall) && fHotplugPending)
        fHotplugPending--;                          /* was queued, will not run */
    while (fHotplugPending)
        IOLockSleep(fLock, &fHotplugPending, THREAD_UNINT);
    IOLockUnlock(fLock);
}

void SAS2308HBA::Hotplug()
{
    for (uint32_t i = 0; i < kMaxTargets; i++) {
        bool create = false, destroy = false;

        IOLockLock(fLock);
        if (fHotplugStop) {
            IOLockUnlock(fLock);
            return;
        }
        Target &t = fTargets[i];
        if (fStarted) {
            if (t.wantDestroy) {
                destroy = t.created;
                t.wantDestroy = false;
            } else if (t.wantCreate && !t.created) {
                create = true;
            }
            t.wantCreate = false;
        }
        IOLockUnlock(fLock);

        if (create) {
            bool ok = CreateTargetForID(i);
            IOLockLock(fLock);
            fTargets[i].created = ok;
            IOLockUnlock(fLock);
            Klog(ok ? (MPSK_MAPPING | MPSK_INFO) : (MPSK_MAPPING | MPSK_ERROR), "CreateTargetForID(%u) %s",
                 i, ok ? "done" : "FAILED");
        } else if (destroy) {
            DestroyTargetForID(i);
            IOLockLock(fLock);
            fTargets[i].created = false;
            IOLockUnlock(fLock);
            Klog(MPSK_MAPPING | MPSK_INFO, "target %u destroyed", i);
        }
    }
}

/* ======================================================================
 * Controller life cycle (mock_ioc_test.cpp Rig order)
 * ==================================================================== */

/* mps_pci_attach() + mps_attach() */
bool SAS2308HBA::InitializeController(void)
{
    fUnit = (uint32_t)OSIncrementAtomic(&gUnits);
    fHost.hba = this;
    fHost.unit = fUnit;
    fHost.setup_interrupts = &SAS2308HBA::HostSetupInterrupts;
    fHost.free_interrupts = &SAS2308HBA::HostFreeInterrupts;
    fHost.pci_restore = &SAS2308HBA::HostPciRestore;
    fHost.controller_dead = &SAS2308HBA::HostControllerDead;
    fHost.io_done_kick = &SAS2308HBA::HostDoneKick;
    fHost.async = &SAS2308HBA::HostAsync;
    ApplyLogSettings();

    fPCI = OSDynamicCast(IOPCIDevice, GetProvider());
    if (!fPCI) {
        Klog(MPSK_FAULT | MPSK_INIT, "provider is not an IOPCIDevice");
        return false;
    }
    fPCI->retain();

    fLock = IOLockAlloc();
    fHotplugCall = thread_call_allocate(&SAS2308HBA::HotplugThread, this);
    fDoneCall = thread_call_allocate(&SAS2308HBA::DoneThread, this);
    if (!fLock || !fHotplugCall || !fDoneCall)
        goto fail;

    /* PCI command register: memory decode + bus mastering (DMA and MSI). */
    fPCI->setMemoryEnable(true);
    fPCI->setBusLeadEnable(true);

    /*
     * SAS2308 BARs: BAR0 = I/O ports (unused), BAR1 = 64-bit memory with
     * the MPI2 system interface registers, BAR3 = 64-bit diagnostic
     * memory. FreeBSD maps PCIR_BAR(1) as well.
     */
    fMap = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    if (!fMap || fMap->getLength() < 0x100) {
        Klog(MPSK_FAULT | MPSK_INIT, "cannot map BAR1");
        goto fail;
    }
    fHost.regs = (volatile uint8_t *)fMap->getVirtualAddress();

    fMapper = IOMapper::copyMapperForDevice(fPCI);
    fHost.mapper = fMapper;
    Klog(MPSK_INIT | MPSK_INFO, "PCI %04x:%04x subsys %04x:%04x, BAR1 %llu bytes mapped, DMA mapper %s",
         fPCI->configRead16(kIOPCIConfigVendorID), fPCI->configRead16(kIOPCIConfigDeviceID),
         fPCI->configRead16(kIOPCIConfigSubSystemVendorID), fPCI->configRead16(kIOPCIConfigSubSystemID),
         (unsigned long long)fMap->getLength(), fMapper ? "present" : "ABSENT (bus address = physical)");

    fMps = mps_kext_create(&fHost, fPCI->configRead16(kIOPCIConfigDeviceID));
    if (!fMps) {
        Klog(MPSK_FAULT | MPSK_INIT, "cannot allocate the softc");
        goto fail;
    }
    {
        /* mps_attach() frees what it allocated on failure; no mps_free() then */
        int rc = mps_kext_attach(fMps, fDebugMask);
        if (rc != 0) {
            Klog(MPSK_FAULT | MPSK_INIT, "mps_attach failed (%d)", rc);
            goto fail;
        }
        IOLockLock(fLock);
        fCoreAttached = true;
        IOLockUnlock(fLock);
    }

    /*
     * Upper-layer transfer cap: 1 MiB (the core's maxio), or less if the
     * IOC's SGL limit (buffer_dmat nsegments: request frame + MaxChainDepth
     * chain frames) could not describe a page-scattered buffer of that size.
     */
    {
        fMaxSegs = mps_kext_max_segs(fMps);
        UInt64 segs = fMaxSegs;
        UInt64 maxBytes = 1024 * 1024;
        if (segs < 2)
            maxBytes = PAGE_SIZE;
        else if ((segs - 1) * PAGE_SIZE < maxBytes)
            maxBytes = (segs - 1) * PAGE_SIZE;
        setProperty(kIOMaximumByteCountReadKey, maxBytes, 64);
        setProperty(kIOMaximumByteCountWriteKey, maxBytes, 64);
        Klog(MPSK_INIT | MPSK_INFO, "max %llu segments per IO, max transfer %llu bytes", segs, maxBytes);
    }
    PublishStats(true);
    return true;

fail:
    ReleaseResources();
    return false;
}

bool SAS2308HBA::StartController(void)
{
    IOLockLock(fLock);
    fStarted = true;
    fStopping = false;
    fHotplugStop = false;
    fDoneStop = false;
    IOLockUnlock(fLock);

    EnableInterrupt();                              /* family: enable the event source */
    if (!Enter()) {
        Klog(MPSK_FAULT | MPSK_INIT, "StartController without an attached core");
        return false;
    }
    mps_kext_start(fMps);                           /* config_intrhook: unmask, port enable (async) */
    Leave();
    DeliverCompletions();
    IOLockLock(fLock);
    ScheduleHotplug();                              /* targets seen before we were started */
    IOLockUnlock(fLock);

    /* Statistics publication (the core's own timers are callouts: mps_periodic, TM timeouts). */
    IOWorkLoop *wl = GetWorkLoop();
    fWatchdog = IOTimerEventSource::timerEventSource(this, &SAS2308HBA::WatchdogFired);
    if (fWatchdog && wl && wl->addEventSource(fWatchdog) == kIOReturnSuccess) {
        fWatchdog->setTimeoutMS(kWatchdogMs);
    } else {
        Klog(MPSK_ERROR | MPSK_INIT, "cannot start the statistics timer");
        OSSafeReleaseNULL(fWatchdog);
    }
    Klog(MPSK_INIT | MPSK_INFO, "controller started");
    return true;
}

/*
 * No new core calls, TM waiters released, running calls finished, then
 * mps_free(): message unit reset (the IOC stops touching our memory), every
 * ccb completed once (CAM_DEV_NOT_THERE), DMA freed.
 */
void SAS2308HBA::StopCore()
{
    if (!fLock)
        return;
    IOLockLock(fLock);
    fStarted = false;
    fStopping = true;
    const bool attached = fCoreAttached;
    IOLockUnlock(fLock);
    if (!attached)
        return;
    mps_kext_stop_waiters(fMps);
    IOLockLock(fLock);
    while (fBusy)
        IOLockSleep(fLock, &fBusy, THREAD_UNINT);
    fCoreAttached = false;
    IOLockUnlock(fLock);
    mps_kext_detach(fMps);
    DeliverCompletions();
}

void SAS2308HBA::StopController(void)
{
    Klog(MPSK_INIT | MPSK_INFO, "stopping controller");
    if (fWatchdog) {
        fWatchdog->cancelTimeout();
        IOWorkLoop *wl = GetWorkLoop();
        if (wl)
            wl->removeEventSource(fWatchdog);
        OSSafeReleaseNULL(fWatchdog);
    }
    DisableInterrupt();
    StopCore();
    QuiesceHotplug();
    QuiesceDone();
    DeliverCompletions();
    LogCounters("stopped");
}

void SAS2308HBA::TerminateController(void)
{
    StopCore();                                     /* if StopController did not run */
    Klog(MPSK_INIT | MPSK_INFO, "terminated: DMA freed, unloading");
    ReleaseResources();
}

void SAS2308HBA::ReleaseResources()
{
    if (fHotplugCall) {
        QuiesceHotplug();
        thread_call_free(fHotplugCall);
        fHotplugCall = nullptr;
    }
    if (fDoneCall) {
        QuiesceDone();
        thread_call_free(fDoneCall);
        fDoneCall = nullptr;
    }
    if (fMps) {
        DeliverCompletions();
        mps_kext_destroy(fMps);                     /* after mps_free or a failed mps_attach */
        fMps = nullptr;
    }
    fHost.regs = nullptr;
    fHost.mapper = nullptr;
    OSSafeReleaseNULL(fMap);
    OSSafeReleaseNULL(fMapper);
    if (fPCI) {
        fPCI->setBusLeadEnable(false);
        OSSafeReleaseNULL(fPCI);
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
}

/* ======================================================================
 * Capabilities
 * ==================================================================== */

SCSIInitiatorIdentifier SAS2308HBA::ReportInitiatorIdentifier(void) { return kMaxTargets; }
SCSIDeviceIdentifier SAS2308HBA::ReportHighestSupportedDeviceID(void) { return kMaxTargets - 1; }
/*
 * The family answers kSCSIProtocolFeature_GetMaximumLogicalUnitNumber with
 * this; IOSCSITargetDevice then issues REPORT LUNS (or probes INQUIRY up to
 * this LUN) and creates the logical units itself. 255 = what peripheral
 * device addressing (MPS_SET_LUN for LUN <= 0xff) covers.
 */
SCSILogicalUnitNumber SAS2308HBA::ReportHBAHighestLogicalUnitNumber(void) { return kMaxLun; }
UInt32 SAS2308HBA::ReportMaximumTaskCount(void) { return kMaxTasks; }
/* Per-task: the XPT_SCSI_IO ccb and its bus_dma_segment_t list (mps_xnu_os.cpp). */
UInt32 SAS2308HBA::ReportHBASpecificTaskDataSize(void) { return (UInt32)mps_kext_task_size(); }
UInt32 SAS2308HBA::ReportHBASpecificDeviceDataSize(void) { return sizeof(UInt64); /* unused */ }
/* We create targets from firmware discovery events, the family must not scan. */
bool SAS2308HBA::DoesHBAPerformDeviceManagement(void) { return true; }
/* The IOC returns sense data with the reply (SYSTEM_SENSE_ADDR). */
bool SAS2308HBA::DoesHBAPerformAutoSense(void) { return true; }
bool SAS2308HBA::DoesHBASupportSCSIParallelFeature(SCSIParallelFeature) { return false; }

bool SAS2308HBA::InitializeTargetForID(SCSITargetIdentifier targetID)
{
    if (targetID >= kMaxTargets || !Enter())
        return false;
    bool present = mps_kext_target_present(fMps, (uint32_t)targetID) != 0;
    Leave();
    return present;
}

static void SetNumber(OSDictionary *dict, const char *key, UInt64 value)
{
    OSNumber *n = OSNumber::withNumber(value, 64);
    if (n) {
        dict->setObject(key, n);
        n->release();
    }
}

void SAS2308HBA::ReportHBAConstraints(OSDictionary *constraints)
{
    const UInt64 segBytes = mps_kext_max_seg_bytes() & ~(UInt64)PAGE_MASK;

    /*
     * What the core can put in one SGL (request frame + chain frames),
     * derived from IOCFacts at attach. The family calls this after
     * InitializeController() (where the core attaches); if it ever came
     * earlier, report 1 (always safe) and say so.
     */
    UInt64 maxSegs = fMaxSegs;
    if (maxSegs == 0) {
        Klog(MPSK_INIT | MPSK_ERROR, "ReportHBAConstraints before attach: reporting 1 segment per IO");
        maxSegs = 1;
    }
    SetNumber(constraints, kIOMaximumSegmentCountReadKey, maxSegs);
    SetNumber(constraints, kIOMaximumSegmentCountWriteKey, maxSegs);
    SetNumber(constraints, kIOMaximumSegmentByteCountReadKey, segBytes);
    SetNumber(constraints, kIOMaximumSegmentByteCountWriteKey, segBytes);
    SetNumber(constraints, kIOMinimumSegmentAlignmentByteCountKey, 4);
    SetNumber(constraints, kIOMaximumSegmentAddressableBitCountKey, 64);
    SetNumber(constraints, kIOMinimumHBADataAlignmentMaskKey, 0xFFFFFFFFFFFFFFF0ull);
}

/*
 * DMA strategy for the data buffers of SCSI tasks: 64-bit IOVAs through
 * the device's DART, segments no longer than one MPI2 simple SGE can
 * describe.
 */
bool SAS2308HBA::InitializeDMASpecification(IODMACommand *command)
{
    if (!command)
        return false;
    if (!fMapper && fPCI)
        fMapper = IOMapper::copyMapperForDevice(fPCI);
    return command->initWithSpecification(
        kIODMACommandOutputHost64, 64, mps_kext_max_seg_bytes() & ~(UInt64)PAGE_MASK,
        IODMACommand::kMapped, 0, 1, fMapper, NULL);
}

/* ======================================================================
 * Interrupts and the statistics timer
 * ==================================================================== */

/*
 * The family would use interrupt index 0, which for a PCI nub is the
 * legacy INTx pin. There is no INTx through a Thunderbolt tunnel on Apple
 * silicon, so look for the message-signalled source (the SAS2308 has both
 * MSI and MSI-X capabilities; IOPCIFamily exposes whichever it set up).
 */
IOInterruptEventSource *SAS2308HBA::CreateDeviceInterrupt(
    IOInterruptEventSource::Action action,
    IOFilterInterruptEventSource::Filter filter,
    IOService *provider)
{
    int index = -1;

    for (int i = 0; i < 64; i++) {
        int type = 0;
        if (provider->getInterruptType(i, &type) != kIOReturnSuccess)
            break;
        if (type & kIOInterruptTypePCIMessaged) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        Klog(MPSK_FAULT | MPSK_INIT, "no MSI/MSI-X interrupt source on the provider, falling back to index 0");
        index = 0;
    } else {
        Klog(MPSK_INIT | MPSK_INFO, "using message-signalled interrupt index %d", index);
    }
    return IOFilterInterruptEventSource::filterInterruptEventSource(this, action, filter, provider, index);
}

/* Primary interrupt context. MSI is ours by construction: always schedule. */
bool SAS2308HBA::FilterInterruptRequest(void)
{
    return true;
}

/* mps_intr_msi(): runs on the work loop. */
void SAS2308HBA::HandleInterruptRequest(void)
{
    if (!Enter())
        return;
    mps_kext_intr(fMps);
    Leave();
    DeliverCompletions();
}

/* Publishes the statistics at most once a second (the core's timers are callouts). */
void SAS2308HBA::WatchdogFired(OSObject *owner, IOTimerEventSource *sender)
{
    SAS2308HBA *self = OSDynamicCast(SAS2308HBA, owner);
    if (!self || !self->fLock)
        return;
    IOLockLock(self->fLock);
    const bool run = self->fStarted && !self->fStopping;
    IOLockUnlock(self->fLock);
    if (!run)
        return;
    self->DeliverCompletions();                     /* belt and braces: nothing should wait here */
    self->PublishStats(false);
    sender->setTimeoutMS(kWatchdogMs);
}

/* ======================================================================
 * Counters -> IORegistry ("SAS2308Statistics"), at most once a second
 * ==================================================================== */

static uint32_t TmSentTotal(const struct mps_xnu_stats &st)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 16; i++) n += st.tm_sent[i];
    return n;
}

/* The core's counters: under mps_mtx while attached, as they were left after mps_free. */
bool SAS2308HBA::Snapshot(struct mps_kext_snapshot *snap)
{
    if (Enter()) {
        mps_kext_snapshot(fMps, snap);
        Leave();
        return true;
    }
    if (!fLock || !fMps)
        return false;
    IOLockLock(fLock);
    const bool detached = !fCoreAttached && !fBusy;
    IOLockUnlock(fLock);
    if (!detached)
        return false;
    mps_kext_snapshot(fMps, snap);
    return true;
}

/*
 * Copy the counters (struct copies), build the dictionary outside every
 * lock. The IO path only increments plain integers; nothing is published
 * per IO.
 */
void SAS2308HBA::PublishStats(bool force)
{
    if (!fLock)
        return;
    const uint64_t now = UptimeUs();
    IOLockLock(fLock);
    if (!force && now - fLastPublishUs < (uint64_t)kStatsPublishMs * 1000) {
        IOLockUnlock(fLock);
        return;
    }
    const KStats ks = fKStats;
    IOLockUnlock(fLock);

    struct mps_kext_snapshot snap;
    if (!Snapshot(&snap))
        return;
    const struct mps_xnu_stats &st = snap.st;
    const bool gone = snap.gone, resetting = snap.resetting;
    const uint32_t mask = fDebugMask;
    const uint64_t sig = st.ios + st.lat_sum_us + st.held + st.io_timeouts + TmSentTotal(st) + st.tm_timeouts +
                         st.faults + st.reinits + st.reinit_failures + st.stale_replies + st.bad_replies + st.events +
                         st.chain_alloc_fail + ks.tasks + ks.tasksRejected + ks.familyTimeouts + ks.tmRequests +
                         snap.held + snap.tms_busy + (gone ? 1 : 0) + (resetting ? 2 : 0) + mask;
    IOLockLock(fLock);
    if (!force && sig == fLastPublishedIos) {
        IOLockUnlock(fLock);
        return;
    }
    fLastPublishUs = now;
    fLastPublishedIos = sig;
    IOLockUnlock(fLock);

    OSDictionary *d = OSDictionary::withCapacity(64);
    if (!d)
        return;
    SetNumber(d, "IOs completed", st.ios);
    SetNumber(d, "Read bytes", st.read_bytes);
    SetNumber(d, "Write bytes", st.write_bytes);
    SetNumber(d, "Latency avg us", st.ios ? st.lat_sum_us / st.ios : 0);
    SetNumber(d, "Latency max us", st.lat_max_us);
    SetNumber(d, "Tasks accepted", ks.tasks);
    SetNumber(d, "Tasks held (recovery)", ks.tasksHeld);
    SetNumber(d, "Tasks rejected", ks.tasksRejected);
    SetNumber(d, "Held now", snap.held);
    SetNumber(d, "Held total", st.held);
    SetNumber(d, "Held timeouts", st.held_timeouts);
    SetNumber(d, "Err check condition", st.err_check_condition);
    SetNumber(d, "Err SCSI status", st.err_scsi_status);
    SetNumber(d, "Err terminated", st.err_terminated);
    SetNumber(d, "Err device gone", st.err_device_gone);
    SetNumber(d, "Err transport", st.err_transport);
    SetNumber(d, "IO timeouts", st.io_timeouts);
    SetNumber(d, "Family timeouts", ks.familyTimeouts);
    SetNumber(d, "TM abort task", st.tm_sent[kTmAbortTask]);
    SetNumber(d, "TM abort task set", st.tm_sent[kTmAbortTaskSet]);
    SetNumber(d, "TM clear task set", st.tm_sent[kTmClearTaskSet]);
    SetNumber(d, "TM clear ACA", st.tm_sent[kTmClearAca]);
    SetNumber(d, "TM LUN reset", st.tm_sent[kTmLunReset]);
    SetNumber(d, "TM target reset", st.tm_sent[kTmTargetReset]);
    SetNumber(d, "TM timeouts", st.tm_timeouts);
    SetNumber(d, "TM late replies", st.tm_late_replies);
    SetNumber(d, "TM failed", st.tm_failed);
    SetNumber(d, "TM in flight", snap.tms_busy);
    SetNumber(d, "TM requests from OS", ks.tmRequests);
    SetNumber(d, "TM requests failed", ks.tmRequestsFailed);
    SetNumber(d, "Recoveries", st.recoveries);
    SetNumber(d, "Recovered by abort", st.recovered_abort);
    SetNumber(d, "Recovered by LUN reset", st.recovered_lun_reset);
    SetNumber(d, "Recovered by target reset", st.recovered_target_reset);
    SetNumber(d, "IOC faults", st.faults);
    SetNumber(d, "Diag resets", st.diag_resets);
    SetNumber(d, "Reinits", st.reinits);
    SetNumber(d, "Reinit failed attempts", st.reinit_failures);
    SetNumber(d, "Reinit max ms", st.reinit_max_ms);
    SetNumber(d, "Controller dead", ks.deadEvents);
    SetNumber(d, "Stale replies", st.stale_replies);
    SetNumber(d, "Bad replies", st.bad_replies);
    SetNumber(d, "Events", st.events);
    SetNumber(d, "Event acks dropped", st.event_ack_dropped);
    SetNumber(d, "Chain alloc failures", st.chain_alloc_fail);
    SetNumber(d, "Chain free low", st.chain_free_low);
    SetNumber(d, "Debug mask", mask);
    OSString *state = OSString::withCString(gone ? "gone" : resetting ? "resetting" : "operational");
    if (state) {
        d->setObject("State", state);
        state->release();
    }
    setProperty(kStatsKey, d);
    d->release();
}

void SAS2308HBA::LogCounters(const char *why)
{
    if (!fLock)
        return;
    struct mps_kext_snapshot snap;
    if (!Snapshot(&snap))
        return;
    IOLockLock(fLock);
    const KStats ks = fKStats;
    IOLockUnlock(fLock);
    const struct mps_xnu_stats &st = snap.st;
    Klog(MPSK_INFO, "%s: %llu IOs (%llu B read, %llu B written), latency avg %llu us max %u us; errors: "
         "check-condition %u, scsi-status %u, terminated %u, device-gone %u, transport %u",
         why, st.ios, st.read_bytes, st.write_bytes, st.ios ? st.lat_sum_us / st.ios : 0, st.lat_max_us,
         st.err_check_condition, st.err_scsi_status, st.err_terminated, st.err_device_gone, st.err_transport);
    Klog(MPSK_INFO, "%s: timeouts %u (family %u), TMs %u (timeouts %u, late %u), recoveries %u, faults %u, "
         "reinits %u (failed attempts %u, max %u ms), stale replies %u",
         why, st.io_timeouts, ks.familyTimeouts, TmSentTotal(st), st.tm_timeouts, st.tm_late_replies, st.recoveries,
         st.faults, st.reinits, st.reinit_failures, st.reinit_max_ms, st.stale_replies);
}

/* ======================================================================
 * IO path
 * ==================================================================== */

/* SCSIParallelTask -> XPT_SCSI_IO ccb (mpssas_action_scsiio via xpt_action). */
SCSIServiceResponse SAS2308HBA::ProcessParallelTask(SCSIParallelTaskIdentifier task)
{
    SCSITargetIdentifier targetID = GetTargetIdentifier(task);
    if (targetID >= kMaxTargets)
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;

    SCSICommandDescriptorBlock cdb = {};
    GetCommandDescriptorBlock(task, &cdb);
    const uint8_t cdbLen = GetCommandDescriptorBlockSize(task);
    if (cdbLen == 0 || cdbLen > sizeof(cdb))
        return kSCSIServiceResponse_FUNCTION_REJECTED;
    const SCSILogicalUnitNumber lun = GetLogicalUnitNumber(task);
    const SCSITaggedTaskIdentifier tag = GetTaggedTaskIdentifier(task);

    UInt64 length = GetRequestedDataTransferCount(task);
    int dir;
    switch (GetDataTransferDirection(task)) {
    case kSCSIDataTransfer_FromTargetToInitiator: dir = MPSK_DIR_IN; break;
    case kSCSIDataTransfer_FromInitiatorToTarget: dir = MPSK_DIR_OUT; break;
    default: dir = MPSK_DIR_NONE; length = 0; break;
    }
    if (dir != MPSK_DIR_NONE && (length == 0 || length > 0xFFFFFFFFull))
        return kSCSIServiceResponse_FUNCTION_REJECTED;

    /*
     * The ccb and its segment list live in the task's HBA data (sized by
     * ReportHBASpecificTaskDataSize()), not on the kernel stack: they stay
     * valid until the task completes, which a held IO needs.
     */
    void *td = GetHBADataPointer(task);
    if (!td || GetHBADataSize(task) < mps_kext_task_size() || fMaxSegs == 0)
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;

    /*
     * bus_dmamap_load_ccb() input (CAM_DATA_SG_PADDR): the family owns the
     * IODMACommand and its memory descriptor; we prepare() the range and
     * complete() it when the IO is done.
     */
    IODMACommand *dma = nullptr;
    uint32_t nsegs = 0;
    if (length) {
        struct mps_kext_seg *segs = mps_kext_task_segs(td);
        const uint64_t maxSegBytes = mps_kext_max_seg_bytes();
        dma = GetDMACommand(task);
        if (!dma || dma->prepare(GetDataBufferOffset(task), length) != kIOReturnSuccess)
            return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;

        UInt64 offset = 0;
        IOReturn ret = kIOReturnSuccess;
        while (offset < length && nsegs < fMaxSegs) {
            IODMACommand::Segment64 chunk[16];
            UInt32 n = fMaxSegs - nsegs;
            if (n > sizeof(chunk) / sizeof(chunk[0]))
                n = sizeof(chunk) / sizeof(chunk[0]);
            ret = dma->gen64IOVMSegments(&offset, chunk, &n);
            if (ret != kIOReturnSuccess || n == 0)
                break;
            for (UInt32 i = 0; i < n; i++) {
                if (chunk[i].fLength == 0 || chunk[i].fLength > maxSegBytes) {
                    ret = kIOReturnInternalError;
                    break;
                }
                segs[nsegs].addr = chunk[i].fIOVMAddr;
                segs[nsegs].len = chunk[i].fLength;
                nsegs++;
            }
            if (ret != kIOReturnSuccess)
                break;
        }
        if (ret != kIOReturnSuccess || offset != length || nsegs == 0) {
            Klog(MPSK_ERROR | MPSK_INFO, "cannot map IO for target %u: ret 0x%x, %u segments for %llu of %llu bytes (cap %u)",
                 (unsigned)targetID, ret, nsegs, offset, length, fMaxSegs);
            dma->complete();
            return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        }
    }

    /* A slot (for AbortTaskRequest) and a pass through the gate. */
    Slot *slot = nullptr;
    IOLockLock(fLock);
    const bool open = !fStopping && fCoreAttached && fMps;
    if (open) {
        for (uint32_t i = 0; i < kMaxTasks && !slot; i++)
            if (!fSlots[i].task)
                slot = &fSlots[i];
    }
    if (slot) {
        slot->task = task;
        slot->prepared = dma != nullptr;
        slot->target = (uint32_t)targetID;
        slot->lun = lun;
        slot->tag = tag;
        fBusy++;
    } else {
        fKStats.tasksRejected++;
    }
    IOLockUnlock(fLock);
    if (!slot) {
        if (dma)
            dma->complete();
        if (open)
            Klog(MPSK_ERROR, "more than %u tasks outstanding: rejecting one", kMaxTasks);
        return open ? kSCSIServiceResponse_FUNCTION_REJECTED : kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }

    struct mps_kext_io_result res;
    const int rc = mps_kext_submit(fMps, td, task, tag, (uint32_t)targetID, lun, cdb, cdbLen, dir, nsegs,
                                   length, &res);

    IOLockLock(fLock);
    if (rc == MPSK_SUBMIT_DONE) {
        *slot = Slot();
        fKStats.tasksRejected++;
    } else {
        fKStats.tasks++;
        if (rc == MPSK_SUBMIT_HELD)
            fKStats.tasksHeld++;
    }
    if (--fBusy == 0 && fStopping)
        IOLockWakeup(fLock, &fBusy, false);
    IOLockUnlock(fLock);

    if (rc == MPSK_SUBMIT_DONE) {
        /* refused by mpssas_action_scsiio (unknown target, gone, invalid, too big) */
        if (dma)
            dma->complete();
        if (res.kind == MPSK_IO_REJECT) {
            Klog(MPSK_ERROR, "IO for target %u refused (CAM status 0x%x)", (unsigned)targetID, res.cam_status);
            return kSCSIServiceResponse_FUNCTION_REJECTED;
        }
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }
    return kSCSIServiceResponse_Request_In_Process;
}

/*
 * mpssas_scsiio_timeout() through mps_xnu_ccb_timeout(). Called on the work
 * loop (gate held) when a task's timer fires. Asynchronous: the core starts
 * (or queues on) its target's recovery - ABORT_TASK, then LUN reset, target
 * reset, controller reinit, each step started from the previous one's
 * completion - and this returns at once. The task is completed
 * (ProtocolTimeoutOccurred) when the IOC gives it back; a task the thin CAM
 * was still holding is completed right away.
 */
void SAS2308HBA::HandleTimeout(SCSIParallelTaskIdentifier task)
{
    if (!Enter()) {
        Klog(MPSK_RECOVERY | MPSK_ERROR, "timeout while stopping: the stop path completes the task");
        return;
    }
    IOLockLock(fLock);
    fKStats.familyTimeouts++;
    IOLockUnlock(fLock);
    const int rc = mps_kext_timeout(fMps, GetHBADataPointer(task), task);
    Leave();
    if (rc != 0)
        Klog(MPSK_RECOVERY | MPSK_ERROR, "timeout for a task that is not outstanding, ignored");
    DeliverCompletions();
}

/* ======================================================================
 * Task management requested by the family (XPT_XNU_TM ccb: the core's
 * mpssas_action_xnu_tm, built like mpssas_send_abort/_reset). The family
 * API is synchronous, the core is not: mps_kext_tm() waits with mps_mtx
 * dropped and pumps the reply queue every 10 ms itself, so completion does
 * not depend on the work loop (the caller may hold its gate). Bounded by
 * the TM timeout (5 s / 30 s) and by StopController.
 * ==================================================================== */

SCSIServiceResponse SAS2308HBA::SendTaskMgmt(uint8_t taskType, SCSITargetIdentifier theT,
                                             SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ)
{
    if (theT >= kMaxTargets)
        return kSCSIServiceResponse_FUNCTION_REJECTED;
    Klog(MPSK_RECOVERY | MPSK_USER, "OS requests %s on target %u lun %llu tag %llu", mps_kext_tm_type_str(taskType),
         (unsigned)theT, (unsigned long long)theL, (unsigned long long)theQ);

    SCSIParallelTaskIdentifier victim = nullptr;
    if (taskType == kTmAbortTask) {
        IOLockLock(fLock);
        for (uint32_t i = 0; i < kMaxTasks && !victim; i++) {
            const Slot &s = fSlots[i];
            if (s.task && s.target == theT && s.lun == theL && s.tag == theQ)
                victim = s.task;
        }
        IOLockUnlock(fLock);
        if (!victim) {
            Klog(MPSK_RECOVERY | MPSK_USER, "abort task on target %u: nothing left to abort", (unsigned)theT);
            return kSCSIServiceResponse_FUNCTION_COMPLETE;
        }
    }
    if (!Enter()) {
        IOLockLock(fLock);
        fKStats.tmRequests++;
        fKStats.tmRequestsFailed++;
        IOLockUnlock(fLock);
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }
    IOLockLock(fLock);
    fKStats.tmRequests++;
    IOLockUnlock(fLock);

    char why[192];
    const int r = mps_kext_tm(fMps, taskType, (uint32_t)theT, theL, victim ? GetHBADataPointer(victim) : nullptr,
                              victim, theQ, why, sizeof(why));
    Leave();

    SCSIServiceResponse response = r == MPSK_TM_COMPLETE ? kSCSIServiceResponse_FUNCTION_COMPLETE :
                                   r == MPSK_TM_REJECTED ? kSCSIServiceResponse_FUNCTION_REJECTED :
                                   kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    Klog(MPSK_RECOVERY | MPSK_USER, "%s on target %u: %s (%s)", mps_kext_tm_type_str(taskType), (unsigned)theT,
         r == MPSK_TM_COMPLETE ? "complete" : r == MPSK_TM_REJECTED ? "rejected" : "failed", why);
    if (response != kSCSIServiceResponse_FUNCTION_COMPLETE) {
        IOLockLock(fLock);
        fKStats.tmRequestsFailed++;
        IOLockUnlock(fLock);
    }
    DeliverCompletions();
    return response;
}

SCSIServiceResponse SAS2308HBA::AbortTaskRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL,
                                                 SCSITaggedTaskIdentifier theQ)
{
    return SendTaskMgmt(kTmAbortTask, theT, theL, theQ);
}
SCSIServiceResponse SAS2308HBA::AbortTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmAbortTaskSet, theT, theL, 0);
}
SCSIServiceResponse SAS2308HBA::ClearACARequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmClearAca, theT, theL, 0);
}
SCSIServiceResponse SAS2308HBA::ClearTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmClearTaskSet, theT, theL, 0);
}
SCSIServiceResponse SAS2308HBA::LogicalUnitResetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmLunReset, theT, theL, 0);
}
SCSIServiceResponse SAS2308HBA::TargetResetRequest(SCSITargetIdentifier theT)
{
    return SendTaskMgmt(kTmTargetReset, theT, 0, 0);
}
