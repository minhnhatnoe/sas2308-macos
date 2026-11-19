/*
 * SAS2308HBA_PM.cpp - power management and shutdown.
 *
 * IOSCSIParallelInterfaceController has no power management of its own
 * (only its target nubs call PMinit, IOSCSIParallelInterfaceDevice.cpp:480),
 * so without this the IOPCIDevice is powered down under a running IOC at
 * system sleep. Two power states, as IntelMausi: off and on (= the
 * IOPCIDevice's kIOPMPowerOn). The provider's doze state (kIOPMSoftSleep,
 * Intel dark wake) has no match here, so it maps to off: the IOC is
 * quiesced, the safe choice. No IOPCIPauseCompatible: a PCI pause (bus
 * renumbering) also takes us through off (IOPCIFamily pause.rtf).
 *
 * Sleep follows Linux mpt3sas scsih_suspend / scsih_resume (FreeBSD's
 * mps_pci_suspend/resume return EINVAL):
 *   off: the core holds new IO (thin CAM, SIM queue frozen) and refuses OS
 *        task management, waits up to kSleepDrainMs for outstanding
 *        commands, stops the IOC with a message unit reset (no DMA after
 *        it), completes what the IOC still owned as a reset (the upper
 *        layers retry, the retry is held), and leaves the registers alone;
 *        then the interrupt source is disabled. The target nubs are our
 *        power children, so the disks have already been spun down / synced
 *        through us before this runs.
 *   on:  interrupt source enabled, the core reinitializes the IOC on its
 *        taskqueue (diag reset, IOCFacts, IOCInit, events, port enable =
 *        rediscovery) and releases the held IO when discovery is done.
 *        Targets that are found again keep their IDs and nubs.
 *
 * Shutdown / restart: the dock has its own power supply, so the IOC keeps
 * running across a Mac restart unless it is stopped. systemWillShutdown
 * does the "off" sequence (shorter drain) and leaves the IOC stopped.
 */
#include "SAS2308HBA.h"

#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>

#define super IOSCSIParallelInterfaceController

static IOPMPowerState sPowerStates[] = {
    /* kPowerStateOff */
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    /* kPowerStateOn */
    { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

#if 0
#pragma mark -
#pragma mark Power management
#endif

//-----------------------------------------------------------------------------
//	InitializePowerManagement - join the power plane under the IOPCIDevice
//	and ask to stay on while the system is awake (no idle power-down: the
//	IOC must keep listening for hotplug events).					[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::InitializePowerManagement(IOService *provider)
{
    if (fPMInitialized || !provider)
        return;
    PMinit();
    provider->joinPMtree(this);
    IOReturn rc = registerPowerDriver(this, sPowerStates, kPowerStateCount);
    if (rc != IOPMNoErr) {
        Klog(MPSK_ERROR | MPSK_INIT, "registerPowerDriver failed (0x%x): no sleep support", rc);
        PMstop();
        return;
    }
    fPowerState = kPowerStateOn;
    fPMInitialized = true;
    changePowerStateTo(kPowerStateOn);
    Klog(MPSK_INIT | MPSK_INFO, "power management: %u states, joined under %s", (unsigned)kPowerStateCount,
         provider->getName());
}

//-----------------------------------------------------------------------------
//	StopPowerManagement - PMstop() once (StopController, TerminateController,
//	free).															[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::StopPowerManagement(void)
{
    if (!fPMInitialized)
        return;
    fPMInitialized = false;
    PMstop();
}

//-----------------------------------------------------------------------------
//	setPowerState - synchronous; returns kIOPMAckImplied.			[PUBLIC]
//-----------------------------------------------------------------------------

IOReturn
SAS2308HBA::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    (void)whatDevice;

    if (!fPMInitialized || powerStateOrdinal >= kPowerStateCount)
        return kIOPMAckImplied;
    if (powerStateOrdinal == fPowerState)
        return kIOPMAckImplied;

    Klog(MPSK_INIT | MPSK_INFO, "power state %lu -> %lu", fPowerState, powerStateOrdinal);
    if (powerStateOrdinal == kPowerStateOff)
        QuiesceController(kSleepDrainMs, "system sleep");
    else if (!fShutdown)
        ResumeController();
    fPowerState = powerStateOrdinal;
    return kIOPMAckImplied;
}

//-----------------------------------------------------------------------------
//	systemWillShutdown - restart or power off: stop the IOC (message unit
//	reset) so it does no DMA into memory the next kernel owns. The dock is
//	separately powered, so nothing else would stop it.				[PUBLIC]
//-----------------------------------------------------------------------------

void
SAS2308HBA::systemWillShutdown(IOOptionBits specifier)
{
    if (specifier == kIOMessageSystemWillPowerOff || specifier == kIOMessageSystemWillRestart) {
        fShutdown = true;
        QuiesceController(kShutdownDrainMs,
                          specifier == kIOMessageSystemWillRestart ? "restart" : "power off");
    }
    super::systemWillShutdown(specifier);
}

//-----------------------------------------------------------------------------
//	QuiesceController - the "off" sequence (mps_xnu_suspend). Returns false
//	if the core was not attached or the IOC did not stop cleanly.	[PRIVATE]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::QuiesceController(uint32_t drainMs, const char *why)
{
    if (fSuspended)
        return true;
    if (!Enter()) {
        Klog(MPSK_INIT | MPSK_INFO, "%s: controller not running, nothing to stop", why);
        return false;
    }
    Klog(MPSK_INIT | MPSK_INFO, "%s: quiescing the controller (drain up to %u ms)", why, drainMs);
    const int rc = mps_kext_suspend(fMps, drainMs);
    Leave();
    DeliverCompletions();                           /* what the IOC owned: reset, retried, then held */
    IOLockLock(fLock);
    fSuspended = true;
    IOLockUnlock(fLock);
    DisableInterrupt();
    Klog(rc ? (MPSK_FAULT | MPSK_INIT) : (MPSK_INIT | MPSK_INFO), "%s: controller stopped%s", why,
         rc ? " (the IOC did not acknowledge the message unit reset)" : " (message unit reset)");
    return rc == 0;
}

//-----------------------------------------------------------------------------
//	ResumeController - the "on" sequence (mps_xnu_resume): the reinit runs
//	on the core's taskqueue; IO stays held until the rediscovery.	[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::ResumeController(void)
{
    if (!fSuspended)
        return;
    EnableInterrupt();
    IOLockLock(fLock);
    fSuspended = false;
    IOLockUnlock(fLock);
    int rc = -1;
    if (Enter()) {
        rc = mps_kext_resume(fMps);
        Leave();
    }
    DeliverCompletions();
    Klog(MPSK_INIT | MPSK_INFO, "wake: %s", rc == 0 ? "reinitializing the controller, IO held until rediscovery"
                                                     : "controller not running");
}
