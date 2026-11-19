/*
 * SAS2308HBA.cpp - IOService life cycle and the IOSCSIParallelInterfaceController
 * callbacks: capabilities, interrupts, IO and task management.
 *
 * Status: compiles and links against the macOS 27 Kernel framework; never
 * loaded or run against hardware. Known gaps are listed in docs/design.md.
 *
 * The core is FreeBSD's sys/dev/mps (core/), reached through
 * mps_xnu_os.{h,cpp}: SCSI tasks become XPT_SCSI_IO ccbs, task-management
 * requests XPT_XNU_TM ccbs, the reinit runs on the core's taskqueue and the
 * timers (mps_periodic, TM and IO timeouts) are the core's callouts.
 *
 * Locking (docs/design.md "Locking rules"):
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
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <IOKit/scsi/SCSITask.h>
#include <libkern/OSAtomic.h>

/* MPI2_SCSITASKMGMT_TASKTYPE_* (core/mpi/mpi2_init.h) */
enum : uint8_t {
    kTmAbortTask    = 0x01,
    kTmAbortTaskSet = 0x02,
    kTmTargetReset  = 0x03,
    kTmLunReset     = 0x05,
    kTmClearTaskSet = 0x06,
    kTmClearAca     = 0x08,
};

static volatile SInt32 gUnits = 0;

#define super IOSCSIParallelInterfaceController
OSDefineMetaClassAndStructors(SAS2308HBA, IOSCSIParallelInterfaceController)

#if 0
#pragma mark -
#pragma mark Core callbacks (mps_pci.c counterparts)
#endif

//-----------------------------------------------------------------------------
//	HostSetupInterrupts - mps_pci_setup_interrupts(): the family owns the
//	interrupt event source (CreateDeviceInterrupt).					[PRIVATE]
//-----------------------------------------------------------------------------

int
SAS2308HBA::HostSetupInterrupts(void *hba)
{
    (void)hba;
    return 0;
}

//-----------------------------------------------------------------------------
//	HostFreeInterrupts - DisableInterrupt() runs in StopController.	[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::HostFreeInterrupts(void *hba)
{
    (void)hba;
}

//-----------------------------------------------------------------------------
//	HostPciRestore - mps_pci_restore() after a diag reset: memory decode and
//	bus mastering (MSI) back on.									[PRIVATE]
//-----------------------------------------------------------------------------

int
SAS2308HBA::HostPciRestore(void *hba)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    if (self->fPCI && !self->fBusLeadOff) {
        self->fPCI->setMemoryEnable(true);
        self->fPCI->setBusLeadEnable(true);
    }
    return 0;
}

//-----------------------------------------------------------------------------
//	HostControllerDead - mps_xnu_disable(): the controller is gone (surprise
//	removal) or disabled (reinit failed); the core fails everything it held.
//	Cut bus mastering so a wedged IOC cannot DMA any more.			[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::HostControllerDead(void *hba)
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

//-----------------------------------------------------------------------------
//	HostDoneKick - completions were queued by a callout or the reinit task:
//	deliver them on fDoneCall.										[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::HostDoneKick(void *hba)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    IOLockLock(self->fLock);
    self->ScheduleDone();
    IOLockUnlock(self->fLock);
}

#if 0
#pragma mark -
#pragma mark The gate around the core, completion delivery
#endif

bool
SAS2308HBA::Enter(void)
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

void
SAS2308HBA::Leave(void)
{
    IOLockLock(fLock);
    if (--fBusy == 0 && fStopping)
        IOLockWakeup(fLock, &fBusy, false);
    IOLockUnlock(fLock);
}

//-----------------------------------------------------------------------------
//	DeliverCompletions - mpssas_scsiio_complete() result -> SAM status for
//	the family. No lock held.										[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::DeliverCompletions(void)
{
    if (!fMps || !fLock)
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
             * Reset (CAM_SCSI_BUS_RESET: reinit, or the IOC stopped for
             * sleep), OS abort (CAM_REQ_ABORTED), transport errors: not the
             * device's fault, a failure the upper layers retry. The core
             * logged the decoded failure (rate-limited).
             */
            status = kSCSITaskStatus_DeliveryFailure;
            response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
            break;
        }
        CompleteParallelTask(task, status, response);
    }
}

void
SAS2308HBA::DoneThread(thread_call_param_t self, thread_call_param_t)
{
    SAS2308HBA *hba = (SAS2308HBA *)self;

    hba->DeliverCompletions();
    IOLockLock(hba->fLock);
    if (--hba->fDonePending == 0)
        IOLockWakeup(hba->fLock, &hba->fDonePending, false);
    IOLockUnlock(hba->fLock);
}

//-----------------------------------------------------------------------------
//	ScheduleDone / QuiesceDone - thread_call_cancel_wait() is only exported
//	by com.apple.kpi.private, which a third-party kext cannot link against,
//	so count the invocations that are queued or running; Quiesce waits for
//	zero. thread_call_enter() returns TRUE if the call was already queued,
//	in which case no additional invocation will happen.				[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::ScheduleDone(void)
{
    if (!fDoneCall || fDoneStop)
        return;
    if (!thread_call_enter(fDoneCall))
        fDonePending++;
}

void
SAS2308HBA::QuiesceDone(void)
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

#if 0
#pragma mark -
#pragma mark Controller life cycle
#endif

//-----------------------------------------------------------------------------
//	InitializeController - mps_pci_attach() + mps_attach(): map BAR1, attach
//	the core (IOC reset, IOCFacts, queues, IOCInit).				[PROTECTED]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::InitializeController(void)
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
    fTunnelled = fPCI->getProperty(kIOPCITunnelledKey) == kOSBooleanTrue;
    Klog(MPSK_INIT | MPSK_INFO, "PCI %04x:%04x subsys %04x:%04x, BAR1 %llu bytes mapped, DMA mapper %s, %s",
         fPCI->configRead16(kIOPCIConfigVendorID), fPCI->configRead16(kIOPCIConfigDeviceID),
         fPCI->configRead16(kIOPCIConfigSubSystemVendorID), fPCI->configRead16(kIOPCIConfigSubSystemID),
         (unsigned long long)fMap->getLength(), fMapper ? "present" : "ABSENT (bus address = physical)",
         fTunnelled ? "Thunderbolt tunnel (IOPCITunnelled)" : "not tunnelled");

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
    PublishControllerCharacteristics();
    PublishStats(true);
    return true;

fail:
    ReleaseResources();
    return false;
}

//-----------------------------------------------------------------------------
//	StartController - config_intrhook (mps_startup: unmask, port enable),
//	power management, statistics timer. Targets are created from the
//	discovery events by the hotplug thread.							[PROTECTED]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::StartController(void)
{
    IOLockLock(fLock);
    fStarted = true;
    fStopping = false;
    fHotplugStop = false;
    fDoneStop = false;
    fTargets.Start();
    IOLockUnlock(fLock);

    EnableInterrupt();                              /* family: enable the event source */
    if (!Enter()) {
        Klog(MPSK_FAULT | MPSK_INIT, "StartController without an attached core");
        return false;
    }
    mps_kext_start(fMps);                           /* config_intrhook: unmask, port enable (async) */
    Leave();
    DeliverCompletions();

    /* Before any target exists, so the target nubs join the power tree below us. */
    InitializePowerManagement(GetProvider());

    IOLockLock(fLock);
    ScheduleHotplug();                              /* targets seen before we were started */
    IOLockUnlock(fLock);

    /* Statistics publication (the core's own timers are callouts: mps_periodic, TM timeouts). */
    IOWorkLoop *wl = GetWorkLoop();
    fStatsTimer = IOTimerEventSource::timerEventSource(this, &SAS2308HBA::StatsTimerFired);
    if (fStatsTimer && wl && wl->addEventSource(fStatsTimer) == kIOReturnSuccess) {
        fStatsTimer->setTimeoutMS(kStatsPublishMs);
    } else {
        Klog(MPSK_ERROR | MPSK_INIT, "cannot start the statistics timer");
        OSSafeReleaseNULL(fStatsTimer);
    }
    Klog(MPSK_INIT | MPSK_INFO, "controller started");
    return true;
}

//-----------------------------------------------------------------------------
//	StopCore - no new core calls, TM waiters released, running calls
//	finished, then mps_free(): message unit reset (the IOC stops touching
//	our memory), every ccb completed once (CAM_DEV_NOT_THERE), DMA freed.
//	Idempotent.														[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::StopCore(void)
{
    if (!fLock)
        return;
    IOLockLock(fLock);
    fStarted = false;
    fStopping = true;
    fTargets.Stop();
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

//-----------------------------------------------------------------------------
//	StopController - called by the family's stop(). Idempotent.	[PROTECTED]
//-----------------------------------------------------------------------------

void
SAS2308HBA::StopController(void)
{
    Klog(MPSK_INIT | MPSK_INFO, "stopping controller");
    StopPowerManagement();                          /* no setPowerState() from here on */
    if (fStatsTimer) {
        fStatsTimer->cancelTimeout();
        IOWorkLoop *wl = GetWorkLoop();
        if (wl)
            wl->removeEventSource(fStatsTimer);
        OSSafeReleaseNULL(fStatsTimer);
    }
    DisableInterrupt();
    StopCore();
    QuiesceHotplug();
    QuiesceDone();
    DeliverCompletions();
    LogCounters("stopped");
}

//-----------------------------------------------------------------------------
//	TerminateController - the family's stop() after StopController, or its
//	start() when StartController failed. Idempotent.				[PROTECTED]
//-----------------------------------------------------------------------------

void
SAS2308HBA::TerminateController(void)
{
    StopPowerManagement();
    StopCore();                                     /* if StopController did not run */
    Klog(MPSK_INIT | MPSK_INFO, "terminated: DMA freed, unloading");
    ReleaseResources();
}

//-----------------------------------------------------------------------------
//	ReleaseResources - everything InitializeController allocated. Safe to
//	call more than once (TerminateController, free).				[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::ReleaseResources(void)
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
        if (!fSuspended)
            fPCI->setBusLeadEnable(false);
        OSSafeReleaseNULL(fPCI);
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
}

//-----------------------------------------------------------------------------
//	willTerminate - the provider is going away (unload, or a Thunderbolt
//	surprise removal). Notice a removal now (all-ones registers: the core
//	fails its IO at once), stop the hotplug thread and wait for a target
//	creation in progress, then let the family destroy the targets; destroy
//	what its loop misses (ID 63, SAS2308HBA_Targets.h).				[PROTECTED]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::willTerminate(IOService *provider, IOOptionBits options)
{
    bool present = true;

    if (Enter()) {
        present = mps_kext_check_present(fMps) != 0;
        Leave();
        DeliverCompletions();
    }
    Klog(MPSK_INIT | MPSK_INFO, "terminating (%s)", present ? "unload or stop" : "controller gone");
    if (fLock) {
        IOLockLock(fLock);
        fTargets.Stop();
        IOLockUnlock(fLock);
    }
    QuiesceHotplug();
    const bool result = super::willTerminate(provider, options);
    TerminateTargets();
    return result;
}

//-----------------------------------------------------------------------------
//	free - last reference gone. ReleaseResources is idempotent, so this is
//	safe after TerminateController and after a failed start.		[PROTECTED]
//-----------------------------------------------------------------------------

void
SAS2308HBA::free(void)
{
    StopPowerManagement();
    ReleaseResources();
    super::free();
}

#if 0
#pragma mark -
#pragma mark Capabilities
#endif

SCSIInitiatorIdentifier SAS2308HBA::ReportInitiatorIdentifier(void) { return kMaxTargets; }
SCSIDeviceIdentifier SAS2308HBA::ReportHighestSupportedDeviceID(void) { return kMaxTargets - 1; }

//-----------------------------------------------------------------------------
//	ReportHBAHighestLogicalUnitNumber - the family answers
//	kSCSIProtocolFeature_GetMaximumLogicalUnitNumber with this;
//	IOSCSITargetDevice then issues REPORT LUNS (or probes INQUIRY up to this
//	LUN) and creates the logical units itself. 255 = what peripheral device
//	addressing (MPS_SET_LUN for LUN <= 0xff) covers.				[PROTECTED]
//-----------------------------------------------------------------------------

SCSILogicalUnitNumber SAS2308HBA::ReportHBAHighestLogicalUnitNumber(void) { return kMaxLun; }
UInt32 SAS2308HBA::ReportMaximumTaskCount(void) { return kMaxTasks; }
/* Per task: the XPT_SCSI_IO ccb and its bus_dma_segment_t list (mps_xnu_os.cpp). */
UInt32 SAS2308HBA::ReportHBASpecificTaskDataSize(void) { return (UInt32)mps_kext_task_size(); }
UInt32 SAS2308HBA::ReportHBASpecificDeviceDataSize(void) { return sizeof(UInt64); /* unused */ }
/* We create targets from firmware discovery events, the family must not scan. */
bool SAS2308HBA::DoesHBAPerformDeviceManagement(void) { return true; }
/* The IOC returns sense data with the reply (SYSTEM_SENSE_ADDR). */
bool SAS2308HBA::DoesHBAPerformAutoSense(void) { return true; }
bool SAS2308HBA::DoesHBASupportSCSIParallelFeature(SCSIParallelFeature) { return false; }

//-----------------------------------------------------------------------------
//	ReportHBAConstraints - what the core can put in one SGL (request frame +
//	chain frames), derived from IOCFacts at attach. The family calls this
//	from AllocateSCSIParallelTasks, after InitializeController
//	(IOSCSIParallelInterfaceController.cpp:313 -> 341 -> 1118). It only
//	dereferences kIOMinimumHBADataAlignmentMaskKey unconditionally (:1157);
//	the other keys have defaults, but we report them all.			[PROTECTED]
//-----------------------------------------------------------------------------

void
SAS2308HBA::ReportHBAConstraints(OSDictionary *constraints)
{
    const UInt64 segBytes = mps_kext_max_seg_bytes() & ~(UInt64)PAGE_MASK;

    /* If it ever came before the attach, report 1 (always safe) and say so. */
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

//-----------------------------------------------------------------------------
//	InitializeDMASpecification - data buffers of SCSI tasks: 64-bit IOVAs
//	through the device's DART, segments no longer than one MPI2 simple SGE
//	can describe.													[PROTECTED]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::InitializeDMASpecification(IODMACommand *command)
{
    if (!command)
        return false;
    if (!fMapper && fPCI)
        fMapper = IOMapper::copyMapperForDevice(fPCI);
    return command->initWithSpecification(
        kIODMACommandOutputHost64, 64, mps_kext_max_seg_bytes() & ~(UInt64)PAGE_MASK,
        IODMACommand::kMapped, 0, 1, fMapper, NULL);
}

#if 0
#pragma mark -
#pragma mark Interrupts
#endif

//-----------------------------------------------------------------------------
//	CreateDeviceInterrupt - the family would use interrupt index 0, which for
//	a PCI nub is the legacy INTx pin. There is no INTx through a Thunderbolt
//	tunnel on Apple silicon, so look for the message-signalled source (the
//	SAS2308 has both MSI and MSI-X capabilities; IOPCIFamily exposes
//	whichever it set up).											[PROTECTED]
//-----------------------------------------------------------------------------

IOInterruptEventSource *
SAS2308HBA::CreateDeviceInterrupt(IOInterruptEventSource::Action action,
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
bool
SAS2308HBA::FilterInterruptRequest(void)
{
    return true;
}

/* mps_intr_msi(): runs on the work loop. */
void
SAS2308HBA::HandleInterruptRequest(void)
{
    if (!Enter())
        return;
    mps_kext_intr(fMps);
    Leave();
    DeliverCompletions();
}

#if 0
#pragma mark -
#pragma mark IO
#endif

//-----------------------------------------------------------------------------
//	ProcessParallelTask - SCSIParallelTask -> XPT_SCSI_IO ccb
//	(mpssas_action_scsiio via xpt_action).							[PROTECTED]
//-----------------------------------------------------------------------------

SCSIServiceResponse
SAS2308HBA::ProcessParallelTask(SCSIParallelTaskIdentifier task)
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

//-----------------------------------------------------------------------------
//	HandleTimeout - mpssas_scsiio_timeout() through mps_xnu_ccb_timeout().
//	Called on the work loop (gate held) when a task's timer fires.
//	Asynchronous: the core starts (or queues on) its target's recovery -
//	ABORT_TASK, then LUN reset, target reset, controller reinit, each step
//	started from the previous one's completion - and this returns at once.
//	The task is completed (ProtocolTimeoutOccurred) when the IOC gives it
//	back; a task the thin CAM was still holding is completed right away.
//																	[PROTECTED]
//-----------------------------------------------------------------------------

void
SAS2308HBA::HandleTimeout(SCSIParallelTaskIdentifier task)
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

#if 0
#pragma mark -
#pragma mark Task management
#endif

//-----------------------------------------------------------------------------
//	SendTaskMgmt - task management requested by the family (XPT_XNU_TM ccb:
//	the core's mpssas_action_xnu_tm, built like mpssas_send_abort/_reset).
//	The family API is synchronous, the core is not: mps_kext_tm() waits with
//	mps_mtx dropped and pumps the reply queue every 10 ms itself, so
//	completion does not depend on the work loop (the caller may hold its
//	gate). Bounded by the TM timeout (5 s / 30 s) and by StopController.
//																	[PRIVATE]
//-----------------------------------------------------------------------------

SCSIServiceResponse
SAS2308HBA::SendTaskMgmt(uint8_t taskType, SCSITargetIdentifier theT,
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
        if (fLock) {
            IOLockLock(fLock);
            fKStats.tmRequests++;
            fKStats.tmRequestsFailed++;
            IOLockUnlock(fLock);
        }
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

SCSIServiceResponse
SAS2308HBA::AbortTaskRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ)
{
    return SendTaskMgmt(kTmAbortTask, theT, theL, theQ);
}

SCSIServiceResponse
SAS2308HBA::AbortTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmAbortTaskSet, theT, theL, 0);
}

SCSIServiceResponse
SAS2308HBA::ClearACARequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmClearAca, theT, theL, 0);
}

SCSIServiceResponse
SAS2308HBA::ClearTaskSetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmClearTaskSet, theT, theL, 0);
}

SCSIServiceResponse
SAS2308HBA::LogicalUnitResetRequest(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
    return SendTaskMgmt(kTmLunReset, theT, theL, 0);
}

SCSIServiceResponse
SAS2308HBA::TargetResetRequest(SCSITargetIdentifier theT)
{
    return SendTaskMgmt(kTmTargetReset, theT, 0, 0);
}
