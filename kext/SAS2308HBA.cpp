/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * SAS2308HBA.cpp - IOService life cycle and the IOSCSIParallelInterfaceController
 * callbacks: capabilities, interrupts, IO and task management.
 *
 * Status: runs an LSI 9207-8i in a Thunderbolt enclosure on macOS 27 (Apple
 * silicon); hardware results and known gaps are in docs/design.md.
 *
 * The core is FreeBSD's sys/dev/mps (core/), reached through
 * mps_xnu_os.{h,cpp}: SCSI tasks become XPT_SCSI_IO ccbs, target and LUN
 * resets XPT_RESET_DEV, aborts the task's timeout recovery, the reinit runs on the core's taskqueue and the
 * timers (mps_periodic, TM and IO timeouts) are the core's callouts.
 *
 * Locking (docs/design.md "Locking rules"):
 *  - mps_mtx (an IOLock, inside the core) protects the core. Every
 *    mps_kext_*() call takes it itself; ccb completions (cbfcnp) and CAM
 *    async callbacks run with it held, so they only queue work: completed
 *    tasks go on a done queue that DeliverCompletions() empties without any
 *    lock, from the thread that made the core call or from fDoneCall.
 *  - fGlue.fLock (an IOLock) protects the glue's tables and flags
 *    (SAS2308Glue.h) and this class's PM flags. It is a leaf: taken inside
 *    mps_mtx by the host callbacks, never held across an mps_kext_*() call
 *    or a family call.
 *  - Nothing that calls back into the SCSI family (CompleteParallelTask,
 *    CreateTargetForID, DestroyTargetForID) runs with either lock held.
 *  - Enter()/Leave() (fGlue) count the threads inside the core; StopController
 *    sets fStopping, wakes the TM waiters and waits for the count to drop to
 *    0 before mps_free().
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
//	HostPciRestore - mps_pci_restore() after a diag reset: FreeBSD's
//	pci_cfg_restore (BARs, command, MSI address/data/enable) is
//	restoreDeviceState() of the config space saved in StartController (D13);
//	memory decode and bus mastering are set again as a fallback.	[PRIVATE]
//-----------------------------------------------------------------------------

int
SAS2308HBA::HostPciRestore(void *hba)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    if (!self->fPCI || self->fBusLeadOff)
        return 0;
    if (self->fPciSaved) {
        const IOReturn r = self->fPCI->restoreDeviceState();
        self->fPciRestores++;
        if (r != kIOReturnSuccess)
            self->fPciRestoreFailures++;
    }
    self->fPCI->setMemoryEnable(true);
    self->fPCI->setBusLeadEnable(true);
    return 0;
}

/* MSI as the config space holds it now (D13 hardware check). */
void
SAS2308HBA::ReadMsi(uint16_t *command, uint16_t *control, uint64_t *address, uint32_t *data)
{
    UInt8 off = 0;

    *command = *control = 0;
    *address = 0;
    *data = 0;
    if (!fPCI)
        return;
    *command = fPCI->configRead16(kIOPCIConfigCommand);
    if (!fPCI->findPCICapability(kIOPCIMSICapability, &off) || off == 0)
        return;
    *control = fPCI->configRead16(off + 2);
    *address = fPCI->configRead32(off + 4);
    if (*control & 0x0080) {                        /* 64-bit address capable */
        *address |= (uint64_t)fPCI->configRead32(off + 8) << 32;
        *data = fPCI->configRead16(off + 12);
    } else {
        *data = fPCI->configRead16(off + 8);
    }
}

//-----------------------------------------------------------------------------
//	HostPciPresent - D11: does the device's config space still answer? An
//	unplugged Thunderbolt device reads all-ones there too.			[PRIVATE]
//-----------------------------------------------------------------------------

int
SAS2308HBA::HostPciPresent(void *hba)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    return self->fPCI && self->fPCI->configRead16(kIOPCIConfigVendorID) == 0x1000;
}

//-----------------------------------------------------------------------------
//	HostGetenv - D33: a FreeBSD loader tunable (hw.mps.X / dev.mps.N.X, asked
//	for as mps.X / mps.N.X): the boot-arg of that name, else the property of
//	that name (the personality's, Info.plist), as a string.		[PRIVATE]
//-----------------------------------------------------------------------------

int
SAS2308HBA::HostGetenv(void *hba, const char *name, char *buf, size_t len)
{
    SAS2308HBA *self = (SAS2308HBA *)hba;
    if (len == 0)
        return 0;
    if (PE_parse_boot_arg_str(name, buf, (int)len))
        return 1;
    OSObject *o = self->getProperty(name);
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) {
        snprintf(buf, len, "%llu", n->unsigned64BitValue());
        return 1;
    }
    if (OSString *s = OSDynamicCast(OSString, o)) {
        strlcpy(buf, s->getCStringNoCopy(), len);
        return 1;
    }
    if (OSBoolean *b = OSDynamicCast(OSBoolean, o)) {
        strlcpy(buf, b->isTrue() ? "1" : "0", len);
        return 1;
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
    self->fGlue.Lock();
    const bool first = !self->fBusLeadOff;
    self->fGlue.fKStats.deadEvents++;
    self->fBusLeadOff = true;
    self->fGlue.Unlock();
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
    self->fGlue.Lock();
    self->fGlue.ScheduleDone();
    self->fGlue.Unlock();
}

#if 0
#pragma mark -
#pragma mark SAS2308GluePort: the family and IOKit for the glue
#endif

bool SAS2308HBAPort::TaskDescribe(void *t, TaskDesc *d) { return fHba->PortTaskDescribe((SCSIParallelTaskIdentifier)t, d); }
bool SAS2308HBAPort::TaskMapBegin(void *t, uint64_t len) { return fHba->PortTaskMapBegin((SCSIParallelTaskIdentifier)t, len); }
bool SAS2308HBAPort::TaskMapNext(void *t, uint64_t *off, struct mps_kext_seg *c, uint32_t *n)
{
    return fHba->PortTaskMapNext((SCSIParallelTaskIdentifier)t, off, c, n);
}
void SAS2308HBAPort::TaskUnmap(void *t) { fHba->PortTaskUnmap((SCSIParallelTaskIdentifier)t); }
/* 0: the task's own timeout (SCSIParallelTimer::SetTimeout); only CompleteParallelTask disarms it */
void SAS2308HBAPort::SetTimeoutForTask(void *t, uint32_t ms) { fHba->SetTimeoutForTask((SCSIParallelTaskIdentifier)t, ms); }
void SAS2308HBAPort::CompleteTask(void *t, int done, uint8_t st, uint64_t xfer, const uint8_t *sense, uint32_t slen)
{
    fHba->PortCompleteTask((SCSIParallelTaskIdentifier)t, done, st, xfer, sense, slen);
}
bool SAS2308HBAPort::CreateTarget(uint32_t id) { return fHba->PortCreateTarget(id); }
void SAS2308HBAPort::DestroyTarget(uint32_t id) { fHba->DestroyTargetForID(id); }
bool SAS2308HBAPort::CallEnter(int which)
{
    thread_call_t c = which == kCallHotplug ? fHba->fHotplugCall : fHba->fDoneCall;
    return c ? thread_call_enter(c) : true;         /* true: no new invocation */
}
bool SAS2308HBAPort::CallCancel(int which)
{
    thread_call_t c = which == kCallHotplug ? fHba->fHotplugCall : fHba->fDoneCall;
    return c ? thread_call_cancel(c) : false;
}
void SAS2308HBAPort::Log(uint32_t cls, const char *line) { fHba->Klog(cls, "%s", line); }

bool
SAS2308HBA::PortTaskDescribe(SCSIParallelTaskIdentifier task, SAS2308GluePort::TaskDesc *d)
{
    SCSICommandDescriptorBlock cdb = {};

    d->target = (uint32_t)GetTargetIdentifier(task);
    GetCommandDescriptorBlock(task, &cdb);
    memcpy(d->cdb, cdb, sizeof(d->cdb));
    d->cdbLen = GetCommandDescriptorBlockSize(task);
    d->lun = GetLogicalUnitNumber(task);
    d->tag = GetTaggedTaskIdentifier(task);
    d->length = GetRequestedDataTransferCount(task);
    switch (GetDataTransferDirection(task)) {
    case kSCSIDataTransfer_FromTargetToInitiator: d->dir = MPSK_DIR_IN; break;
    case kSCSIDataTransfer_FromInitiatorToTarget: d->dir = MPSK_DIR_OUT; break;
    default: d->dir = MPSK_DIR_NONE; break;
    }
    d->td = GetHBADataPointer(task);
    d->tdSize = GetHBADataSize(task);
    d->timeoutMs = GetTimeoutDuration(task);
    return GetTargetIdentifier(task) < kMaxTargets;
}

/* the family owns the IODMACommand and its memory descriptor: prepare the range, list it, complete it */
bool
SAS2308HBA::PortTaskMapBegin(SCSIParallelTaskIdentifier task, uint64_t length)
{
    IODMACommand *dma = GetDMACommand(task);
    return dma && dma->prepare(GetDataBufferOffset(task), length) == kIOReturnSuccess;
}

bool
SAS2308HBA::PortTaskMapNext(SCSIParallelTaskIdentifier task, uint64_t *offset, struct mps_kext_seg *chunk,
                            uint32_t *n)
{
    IODMACommand *dma = GetDMACommand(task);
    IODMACommand::Segment64 seg[16];
    UInt32 count = *n > 16 ? 16 : *n;
    UInt64 off = *offset;

    if (!dma || dma->gen64IOVMSegments(&off, seg, &count) != kIOReturnSuccess)
        return false;
    for (UInt32 i = 0; i < count; i++) {
        chunk[i].addr = seg[i].fIOVMAddr;
        chunk[i].len = seg[i].fLength;
    }
    *offset = off;
    *n = count;
    return true;
}

void
SAS2308HBA::PortTaskUnmap(SCSIParallelTaskIdentifier task)
{
    IODMACommand *dma = GetDMACommand(task);
    if (dma)
        dma->complete();
}

//-----------------------------------------------------------------------------
//	PortCompleteTask - mpssas_scsiio_complete() result -> SAM status for
//	the family. No lock held.										[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::PortCompleteTask(SCSIParallelTaskIdentifier task, int done, uint8_t scsiStatus, uint64_t transferred,
                             const uint8_t *sense, uint32_t senseLen)
{
    SCSITaskStatus status;
    SCSIServiceResponse response = kSCSIServiceResponse_TASK_COMPLETE;

    switch (done) {
    case SAS2308GluePort::kDoneStatus:
        /* CAM_REQ_CMP / CAM_SCSI_STATUS_ERROR: the SAM status byte, same encoding as SCSITaskStatus */
        status = (SCSITaskStatus)scsiStatus;
        SetRealizedDataTransferCount(task, transferred);
        if (senseLen)
            SetAutoSenseData(task, (SCSI_Sense_Data *)sense, (UInt8)senseLen);
        break;
    case SAS2308GluePort::kDoneGone:
        status = kSCSITaskStatus_DeviceNotPresent;
        response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        break;
    case SAS2308GluePort::kDoneTimeout:
        /*
         * terminated by the recovery after the family's timeout
         * (CAM_CMD_TIMEOUT): the family's own status for its own timer, as
         * its default HandleTimeout (D44)
         */
        status = kSCSITaskStatus_TaskTimeoutOccurred;
        response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        break;
    case SAS2308GluePort::kDoneReject:
        /* refused by mpssas_action_scsiio (invalid, too big): synchronous FUNCTION_REJECTED, as the pre-FreeBSD driver */
        status = kSCSITaskStatus_No_Status;
        response = kSCSIServiceResponse_FUNCTION_REJECTED;
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

void
SAS2308HBA::DoneThread(thread_call_param_t self, thread_call_param_t)
{
    ((SAS2308HBA *)self)->fGlue.DoneCallBody();
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
    fHost.pci_present = &SAS2308HBA::HostPciPresent;
    fHost.getenv = &SAS2308HBA::HostGetenv;
    fHost.io_done_kick = &SAS2308HBA::HostDoneKick;
    fHost.async = &SAS2308HBA::HostAsync;
    ApplyLogSettings();

    fPCI = OSDynamicCast(IOPCIDevice, GetProvider());
    if (!fPCI) {
        Klog(MPSK_FAULT | MPSK_INIT, "provider is not an IOPCIDevice");
        return false;
    }
    fPCI->retain();

    fGluePort.fHba = this;
    fHotplugCall = thread_call_allocate(&SAS2308HBA::HotplugThread, this);
    fDoneCall = thread_call_allocate(&SAS2308HBA::DoneThread, this);
    if (!fGlue.Init(&fGluePort) || !fHotplugCall || !fDoneCall)
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

    fGlue.fMps = mps_kext_create(&fHost, fPCI->configRead16(kIOPCIConfigDeviceID));
    if (!fGlue.fMps) {
        Klog(MPSK_FAULT | MPSK_INIT, "cannot allocate the softc");
        goto fail;
    }
    {
        /* mps_attach() frees what it allocated on failure; no mps_free() then */
        int rc = mps_kext_attach(fGlue.fMps, fDebugMask);
        if (rc != 0) {
            Klog(MPSK_FAULT | MPSK_INIT, "mps_attach failed (%d)", rc);
            goto fail;
        }
        if (!fGlue.Attached()) {
            mps_kext_detach(fGlue.fMps);
            goto fail;
        }
    }

    /*
     * Upper-layer transfer cap: 1 MiB (the core's maxio), or less if the
     * IOC's SGL limit (buffer_dmat nsegments: request frame + MaxChainDepth
     * chain frames) could not describe a page-scattered buffer of that size.
     */
    {
        UInt64 segs = fGlue.fMaxSegs;
        UInt64 maxBytes = 1024 * 1024;
        if (segs < 2)
            maxBytes = PAGE_SIZE;
        else if ((segs - 1) * PAGE_SIZE < maxBytes)
            maxBytes = (segs - 1) * PAGE_SIZE;
        setProperty(kIOMaximumByteCountReadKey, maxBytes, 64);
        setProperty(kIOMaximumByteCountWriteKey, maxBytes, 64);
        Klog(MPSK_INIT | MPSK_INFO, "max %llu segments per IO, max transfer %llu bytes, %u commands",
             segs, maxBytes, fGlue.fMaxTasks);
    }
    PublishControllerCharacteristics();
    PublishLimits(true);
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
    fGlue.Start();

    EnableInterrupt();                              /* family: enable the event source */
    /*
     * D13: keep the config space as it is now (BARs, command, MSI set up by
     * the interrupt source) for HostPciRestore, as FreeBSD's
     * pci_cfg_save/restore. kIOPCIConfigShadowPermanent: save only (no
     * power state change), and the copy stays valid across restores.
     */
    if (fPCI) {
        const IOReturn r = fPCI->saveDeviceState(kIOPCIConfigShadowPermanent);
        fPciSaved = r == kIOReturnSuccess;
        uint16_t cmd, ctl;
        uint64_t addr;
        uint32_t data;
        ReadMsi(&cmd, &ctl, &addr, &data);
        Klog(fPciSaved ? (MPSK_INIT | MPSK_INFO) : (MPSK_INIT | MPSK_ERROR),
             "PCI config space %s (0x%x): command 0x%04x, MSI control 0x%04x address 0x%llx data 0x%04x",
             fPciSaved ? "saved" : "not saved", r, cmd, ctl, (unsigned long long)addr, data);
    }
    if (!Enter()) {
        Klog(MPSK_FAULT | MPSK_INIT, "StartController without an attached core");
        return false;
    }
    mps_kext_start(fGlue.fMps);                     /* config_intrhook: unmask, port enable (async) */
    Leave();
    DeliverCompletions();

    /* Before any target exists, so the target nubs join the power tree below us. */
    InitializePowerManagement(GetProvider());

    fGlue.Lock();
    fGlue.ScheduleHotplug();                        /* targets seen before we were started */
    fGlue.Unlock();

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
    /*
     * Interrupt off before mps_free, the reverse of FreeBSD's mps_pci_detach
     * (mps_free, then the interrupt teardown): the family owns the source
     * and we have no bus_teardown_intr that would wait for a running
     * mps_intr after the queues are freed. What mps_free waits for
     * completes without it (config waits: ENXIO short-cut; TMs:
     * mpssas_complete_all_commands), and it drains the reply queue once,
     * polled, before the MUR (D51).
     */
    DisableInterrupt();
    fGlue.StopCore();                               /* no new core calls, mps_free: every ccb completed */
    fGlue.QuiesceHotplug();
    fGlue.QuiesceDone();
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
    fGlue.StopCore();                               /* if StopController did not run */
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
        fGlue.QuiesceHotplug();
        thread_call_free(fHotplugCall);
        fHotplugCall = nullptr;
    }
    if (fDoneCall) {
        fGlue.QuiesceDone();
        thread_call_free(fDoneCall);
        fDoneCall = nullptr;
    }
    if (fGlue.fMps) {
        DeliverCompletions();
        mps_kext_destroy(fGlue.fMps);               /* after mps_free or a failed mps_attach */
        fGlue.fMps = nullptr;
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
    fGlue.Fini();
}

//-----------------------------------------------------------------------------
//	willTerminate - the provider is going away (unload, or a Thunderbolt
//	surprise removal). SAS2308Glue::WillTerminate notices a removal now
//	(all-ones registers), disables the controller synchronously and delivers
//	every completion, so no IO is left on a nub; it stops the hotplug thread
//	and waits for a target creation in progress. Then the family destroys
//	the targets; we destroy what its loop misses (ID 63,
//	SAS2308HBA_Targets.h).							[PROTECTED]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::willTerminate(IOService *provider, IOOptionBits options)
{
    const bool present = fGlue.WillTerminate();

    Klog(MPSK_INIT | MPSK_INFO, "terminating (%s)", present ? "unload or stop" : "controller gone");
    const bool result = super::willTerminate(provider, options);
    fGlue.TerminateTargets((uint32_t)ReportHighestSupportedDeviceID());
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

//-----------------------------------------------------------------------------
//	ReportMaximumTaskCount - min(IOCFacts RequestCredit, MPS_REQ_FRAMES 2048,
//	post queue depth) less the internal commands (mps_resize_queues; one glue
//	slot per task). The family asks right after
//	InitializeController (IOSCSIParallelInterfaceController.cpp:337), sizes
//	its task pool with it and, since we do not set it, publishes
//	IOCommandPoolSize = the tasks it allocated.				[PROTECTED]
//-----------------------------------------------------------------------------

UInt32
SAS2308HBA::ReportMaximumTaskCount(void)
{
    if (fGlue.fMaxTasks == 0) {
        Klog(MPSK_INIT | MPSK_ERROR, "ReportMaximumTaskCount before attach: reporting 1 task");
        return 1;
    }
    return fGlue.fMaxTasks;
}

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
    UInt64 maxSegs = fGlue.fMaxSegs;
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
    mps_kext_intr(fGlue.fMps);
    Leave();
    DeliverCompletions();
}

#if 0
#pragma mark -
#pragma mark IO
#endif

//-----------------------------------------------------------------------------
//	ProcessParallelTask - SCSIParallelTask -> XPT_SCSI_IO ccb
//	(mpssas_action_scsiio via xpt_action): SAS2308Glue::ProcessTask.
//																	[PROTECTED]
//-----------------------------------------------------------------------------

SCSIServiceResponse
SAS2308HBA::ProcessParallelTask(SCSIParallelTaskIdentifier task)
{
    switch (fGlue.ProcessTask(task)) {
    case SAS2308Glue::kInProcess:
        return kSCSIServiceResponse_Request_In_Process;
    case SAS2308Glue::kRejected:
        return kSCSIServiceResponse_FUNCTION_REJECTED;
    default:
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }
}

//-----------------------------------------------------------------------------
//	HandleTimeout - mpssas_scsiio_timeout() through mps_xnu_ccb_timeout().
//	Called on the work loop (gate held) when a task's timer fires (only
//	tasks given to SetTimeoutForTask are timed). Asynchronous: the core
//	starts (or queues on) its target's recovery - ABORT_TASK, then LUN
//	reset, target reset, controller reinit - and this returns at once. The
//	task is completed (TaskTimeoutOccurred) when the IOC gives it back.
//	A task still held, or posted less than its timeout ago, is re-armed for
//	the rest (D4: FreeBSD's callout runs from the post).
//																	[PROTECTED]
//-----------------------------------------------------------------------------

void
SAS2308HBA::HandleTimeout(SCSIParallelTaskIdentifier task)
{
    fGlue.HandleTimeout(task);
}

#if 0
#pragma mark -
#pragma mark Task management
#endif

//-----------------------------------------------------------------------------
//	SendTaskMgmt - task management requested by the family, mapped as
//	FreeBSD does (D10, mps_kext_tm): target/LUN reset -> XPT_RESET_DEV,
//	abort -> the task's timeout recovery, the rest rejected. The family API
//	is synchronous, the core is not: mps_kext_tm() waits with mps_mtx
//	dropped and pumps the reply queue every 10 ms itself, so completion
//	does not depend on the work loop (the caller may hold its gate).
//	Bounded by the recovery (TM timeouts, reinit) and by StopController.
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
        victim = (SCSIParallelTaskIdentifier)fGlue.FindTask((uint32_t)theT, theL, theQ);
        if (!victim) {
            Klog(MPSK_RECOVERY | MPSK_USER, "abort task on target %u: nothing left to abort", (unsigned)theT);
            return kSCSIServiceResponse_FUNCTION_COMPLETE;
        }
    }
    if (!Enter()) {
        if (fGlue.HasLock()) {
            fGlue.Lock();
            fGlue.fKStats.tmRequests++;
            fGlue.fKStats.tmRequestsFailed++;
            fGlue.Unlock();
        }
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }
    fGlue.Lock();
    fGlue.fKStats.tmRequests++;
    fGlue.Unlock();

    char why[192];
    const int r = mps_kext_tm(fGlue.fMps, taskType, (uint32_t)theT, theL, victim ? GetHBADataPointer(victim) : nullptr,
                              victim, theQ, why, sizeof(why));
    Leave();

    SCSIServiceResponse response = r == MPSK_TM_COMPLETE ? kSCSIServiceResponse_FUNCTION_COMPLETE :
                                   r == MPSK_TM_REJECTED ? kSCSIServiceResponse_FUNCTION_REJECTED :
                                   kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    Klog(MPSK_RECOVERY | MPSK_USER, "%s on target %u: %s (%s)", mps_kext_tm_type_str(taskType), (unsigned)theT,
         r == MPSK_TM_COMPLETE ? "complete" : r == MPSK_TM_REJECTED ? "rejected" : "failed", why);
    if (response != kSCSIServiceResponse_FUNCTION_COMPLETE) {
        fGlue.Lock();
        fGlue.fKStats.tmRequestsFailed++;
        fGlue.Unlock();
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
