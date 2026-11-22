/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 *
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
 *        taskqueue with FreeBSD's attach sequence (D12: a READY IOC is not
 *        reset; IOCFacts, IOCInit, events, port enable = rediscovery), the
 *        diag reset only as the fallback, and releases the held IO when
 *        discovery is done.
 *        Targets that are found again keep their IDs and nubs.
 *
 * Shutdown / restart: the dock has its own power supply, so the IOC keeps
 * running across a Mac restart: as FreeBSD, systemWillShutdown does not
 * stop it and the next attach takes it over with a MUR (D50);
 * sas2308_shutdown_stop 1 does the "off" sequence (shorter drain) instead.
 *
 * The halt (batch F): PEHaltRestartInternal arms a 30 s watchdog ("Halt/
 * Restart Timed Out" panic) and then IOPMrootDomain::handlePlatformHaltRestart
 * messages the priority power interest clients (kIOMessageSystemWillRestart
 * / WillPowerOff; SAM's block driver marks "Power Off" there) before it
 * walks the power tree with systemWillShutdown, children (the disks) first.
 * Our priority interest handler is the earliest point inside that window:
 * from there every command has at most 15 s left (mps_xnu_halt_begin), so a
 * lost reply to a command in flight, or to one SAM sends during the halt
 * (START STOP UNIT, timeout 0), is recovered and failed before the
 * watchdog fires. Sleep keeps D15's backstop: it has no such watchdog.
 */
#include "SAS2308HBA.h"

#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>

/*
 * IOKit/pwr_mgt/RootDomain.h declares it but does not compile outside xnu
 * (IOPMDriverAssertionID is in IOPMPrivate.h): the KPI function, as there.
 */
extern "C" IONotifier *registerPrioritySleepWakeInterest(IOServiceInterestHandler handler, void *self, void *ref);

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
        Klog(MPSK_ERROR | MPSK_INFO | MPSK_INIT, "registerPowerDriver failed (0x%x): no sleep support", rc);
        PMstop();
        return;
    }
    fPowerState = kPowerStateOn;
    fPMInitialized = true;
    fHaltNotifier = registerPrioritySleepWakeInterest(&SAS2308HBA::HaltMessage, this, nullptr);
    if (!fHaltNotifier)     /* systemWillShutdown still caps, later in the halt; every stop is sent (S1) */
        Klog(MPSK_ERROR | MPSK_INIT, "no priority power interest: the halt's timeout caps start late");
    else
        SystemPower(MPSK_SYS_AWAKE);
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
    if (fHaltNotifier) {
        fHaltNotifier->remove();    /* waits for a handler that runs */
        fHaltNotifier = nullptr;
    }
    PMstop();
}

//-----------------------------------------------------------------------------
//	setPowerState - synchronous; returns kIOPMAckImplied.			[PUBLIC]
//	Synchronous on purpose (D49): drain + MUR + reinit measured well under
//	a second (498 ms wake, measured on the test setup), and the bounded worst case (10 s
//	drain + the stop) is inside the PM thread's tolerance; an asynchronous
//	ack would add a second state machine for no measured benefit.
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
        QuiesceController(kSleepDrainMs, "system sleep", false);
    else if (!fShutdown)
        ResumeController();
    fPowerState = powerStateOrdinal;
    return kIOPMAckImplied;
}

//-----------------------------------------------------------------------------
//	HaltMessage - priority power interest (registerPrioritySleepWakeInterest):
//	restart / power off begins, the 30 s halt watchdog runs. Sleep messages
//	need no answer here (SAM's handler is the same kind). The root domain
//	tells its priority clients before the power tree goes down, so
//	WillSleep arrives before SAM's system-sleep stop: from there until the
//	wake every START STOP UNIT is sent (sas2308_idle_spindown, S1).	[PRIVATE]
//-----------------------------------------------------------------------------

IOReturn
SAS2308HBA::HaltMessage(void *target, void *refCon, UInt32 messageType, IOService *provider,
                        void *messageArgument, vm_size_t argSize)
{
    (void)refCon; (void)provider; (void)messageArgument; (void)argSize;
    SAS2308HBA *me = (SAS2308HBA *)target;
    if (messageType == kIOMessageSystemWillRestart || messageType == kIOMessageSystemWillPowerOff)
        me->BeginHalt(messageType == kIOMessageSystemWillRestart ? "restart" : "power off");
    else if (messageType == kIOMessageSystemWillSleep)
        me->SystemPower(MPSK_SYS_SLEEP);
    else if (messageType == kIOMessageSystemWillPowerOn || messageType == kIOMessageSystemHasPoweredOn)
        me->SystemPower(MPSK_SYS_AWAKE);
    return kIOReturnSuccess;
}

//-----------------------------------------------------------------------------
//	SystemPower - the system's power state for SAM's idle stop (S1).	[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::SystemPower(int state)
{
    if (!Enter())
        return;
    mps_kext_system_power(fGlue.fMps, state);
    Leave();
}

//-----------------------------------------------------------------------------
//	BeginHalt - cap every command's time left (mps_kext_halt_begin). Once;
//	from the priority interest, else from systemWillShutdown.		[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::BeginHalt(const char *why)
{
    if (!Enter())
        return;
    Klog(MPSK_INIT | MPSK_INFO, "%s: halt begins, command timeouts capped at 15 s", why);
    mps_kext_halt_begin(fGlue.fMps);
    Leave();
}

//-----------------------------------------------------------------------------
//	systemWillShutdown - restart or power off. As FreeBSD (mps registers only
//	shutdown_final -> mpssas_ir_shutdown: IR volumes, none on IT firmware;
//	the IOC is stopped only at detach), the IOC keeps running: IO is served
//	until the power goes, under the halt's caps (batch F). The next attach
//	takes over an OPERATIONAL IOC with a MUR before any queue is set up
//	(mps_transition_ready, FreeBSD's attach). D50: see the deviations plan,
//	batch G2. sas2308_shutdown_stop 1 restores the old stop (MUR).	[PUBLIC]
//-----------------------------------------------------------------------------

void
SAS2308HBA::systemWillShutdown(IOOptionBits specifier)
{
    if (specifier == kIOMessageSystemWillPowerOff || specifier == kIOMessageSystemWillRestart) {
        const char *why = specifier == kIOMessageSystemWillRestart ? "restart" : "power off";
        if (!fHaltNotifier)
            BeginHalt(why);
        if (fShutdownStop) {
            fShutdown = true;
            /*
             * Not sleep: nothing resumes, so IO is failed (DeviceNotPresent)
             * from here on instead of held, and the stop is bounded (~3 s)
             * well inside the 30 s halt watchdog: no diag reset, one MUR.
             */
            QuiesceController(kShutdownDrainMs, why, true);
        } else {
            Klog(MPSK_INIT | MPSK_INFO, "%s: controller left running (FreeBSD: no stop at shutdown)", why);
        }
    }
    super::systemWillShutdown(specifier);
}

//-----------------------------------------------------------------------------
//	QuiesceController - the "off" sequence (mps_xnu_suspend). Returns false
//	if the core was not attached or the IOC did not stop cleanly.	[PRIVATE]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::QuiesceController(uint32_t drainMs, const char *why, bool shutdown)
{
    if (fSuspended)
        return true;
    if (!Enter()) {
        Klog(MPSK_INIT | MPSK_INFO, "%s: controller not running, nothing to stop", why);
        return false;
    }
    Klog(MPSK_INIT | MPSK_INFO, "%s: quiescing the controller (drain up to %u ms)", why, drainMs);
    const int rc = mps_kext_suspend(fGlue.fMps, drainMs, shutdown ? 1 : 0);
    Leave();
    DeliverCompletions();                           /* what the IOC owned: reset, retried, then held */
    fGlue.Lock();
    fSuspended = true;
    fGlue.Unlock();
    DisableInterrupt();
    /*
     * Marked suspended even when the IOC did not stop (the core's fault
     * line and "Sleep stop failures" say so, D49): the wake's reinit is
     * what recovers it; a shutdown leaves it to the next boot.
     */
    if (rc)
        Klog(MPSK_INIT | MPSK_ERROR, "%s: the IOC did not stop cleanly (error %d): marked stopped, %s", why, rc,
             shutdown ? "the next attach resets it" : "the wake reinitializes it");
    else
        Klog(MPSK_INIT | MPSK_INFO, "%s: controller stopped (message unit reset)", why);
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
    fGlue.Lock();
    fSuspended = false;
    fGlue.Unlock();
    int rc = -1;
    if (Enter()) {
        rc = mps_kext_resume(fGlue.fMps);
        Leave();
    }
    DeliverCompletions();
    if (rc == 0)
        Klog(MPSK_INIT | MPSK_INFO, "wake: reinitializing the controller, IO held until rediscovery");
    else            /* a resume that could not run: the disks stay unreachable (D49) */
        Klog(MPSK_FAULT | MPSK_INIT, "wake: the controller could not be resumed (error %d): not running", rc);
}
