/*
 * SAS2308HBA_Hotplug.cpp - SCSI target nubs: created and destroyed from the
 * core's discovery events, their properties, and their teardown when the
 * controller terminates.
 *
 * Target creation runs on its own thread call: CreateTargetForID() probes
 * the device synchronously, and those probe IOs complete on the work loop.
 * The bookkeeping is SAS2308TargetTable (SAS2308HBA_Targets.h, host-tested).
 *
 * Per-target properties, as the family and Apple's drivers expect them:
 *   - "SAS Address" (kIOPropertySASAddressKey, 8 bytes, big-endian) from
 *     SAS Device Page 0, passed to CreateTargetForID(id, dict): the family
 *     puts it in the nub's Protocol Characteristics, adds "Unique SCSI Port
 *     Identifier" and names the nub "SAS Target <address>"
 *     (IOSCSIParallelInterfaceDevice.cpp:1113-1124).
 *   - "Physical Interconnect" / "Physical Interconnect Location" and the
 *     read/write timeouts in the nub's Protocol Characteristics, set from
 *     InitializeTargetForID (called by the nub's start() before it is
 *     registered). The family builds a fresh dictionary for each nub and
 *     does not copy the controller's (IOSCSIParallelInterfaceDevice.cpp:
 *     129-163); iSCSIInitiator (iSCSIVirtualHBA.cpp:150-162) does the same.
 */
#include "SAS2308HBA.h"

#include <IOKit/storage/IOStorageControllerCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <IOKit/scsi/IOSCSIProtocolInterface.h>
#include <libkern/OSByteOrder.h>

#if 0
#pragma mark -
#pragma mark Discovery events
#endif

//-----------------------------------------------------------------------------
//	HostAsync - CAM async events (mps_mtx held). `present' was sampled
//	(mps_xnu_target_present) when the event happened; the hotplug thread
//	acts on it later. A rescan finds a target present -> create it; absent
//	(removed, or not rediscovered after a reinit: the core rescans the whole
//	bus when port enable completes) -> destroy it. AC_BUS_RESET alone changes
//	nothing (targets survive a reinit if the rediscovery finds them again).
//																	[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::HostAsync(void *hba, int event, uint64_t tids, uint64_t present)
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
    IOLockLock(self->fLock);
    if (self->fTargets.Update(tids, present))
        self->ScheduleHotplug();
    IOLockUnlock(self->fLock);
}

#if 0
#pragma mark -
#pragma mark The hotplug thread
#endif

void
SAS2308HBA::HotplugThread(thread_call_param_t self, thread_call_param_t)
{
    SAS2308HBA *hba = (SAS2308HBA *)self;

    hba->Hotplug();
    IOLockLock(hba->fLock);
    if (--hba->fHotplugPending == 0)
        IOLockWakeup(hba->fLock, &hba->fHotplugPending, false);
    IOLockUnlock(hba->fLock);
}

void
SAS2308HBA::ScheduleHotplug(void)
{
    if (!fHotplugCall || fHotplugStop)
        return;
    if (!thread_call_enter(fHotplugCall))
        fHotplugPending++;
}

//-----------------------------------------------------------------------------
//	QuiesceHotplug - no new invocations; wait for the running one, which
//	finishes the CreateTargetForID()/DestroyTargetForID() it is in. [PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::QuiesceHotplug(void)
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

void
SAS2308HBA::Hotplug(void)
{
    uint32_t cursor = 0;

    for (;;) {
        SAS2308TargetTable::Work w = { SAS2308TargetTable::kNone, 0 };
        IOLockLock(fLock);
        if (!fHotplugStop)
            w = fTargets.Next(cursor);
        IOLockUnlock(fLock);
        if (w.action == SAS2308TargetTable::kNone)
            break;

        bool ok = true;
        if (w.action == SAS2308TargetTable::kCreate) {
            if (isInactive()) {
                ok = false;                         /* terminating: never attach a new nub */
                Klog(MPSK_MAPPING | MPSK_INFO, "target %u: not created, controller terminating", w.targetID);
            } else {
                OSDictionary *props = CopyTargetProperties(w.targetID);
                ok = props ? CreateTargetForID(w.targetID, props) : CreateTargetForID(w.targetID);
                OSSafeReleaseNULL(props);
                Klog(ok ? (MPSK_MAPPING | MPSK_INFO) : (MPSK_MAPPING | MPSK_ERROR), "CreateTargetForID(%u) %s",
                     w.targetID, ok ? "done" : "FAILED");
            }
        } else {
            DestroyTargetForID(w.targetID);
            Klog(MPSK_MAPPING | MPSK_INFO, "target %u destroyed", w.targetID);
        }
        IOLockLock(fLock);
        fTargets.Done(w, ok);
        IOLockUnlock(fLock);
    }
}

#if 0
#pragma mark -
#pragma mark Target properties
#endif

//-----------------------------------------------------------------------------
//	CopyTargetProperties - the CreateTargetForID() dictionary: the SAS
//	address from SAS Device Page 0, big-endian as the family requires
//	(kSASAddressDataSize = 8). NULL if the core has none.			[PRIVATE]
//-----------------------------------------------------------------------------

OSDictionary *
SAS2308HBA::CopyTargetProperties(SCSITargetIdentifier targetID)
{
    struct mps_kext_target_info ti;
    int found = 0;

    if (Enter()) {
        found = mps_kext_target_info(fMps, (uint32_t)targetID, &ti);
        Leave();
    }
    if (!found || ti.sas_address == 0)
        return nullptr;

    OSDictionary *dict = OSDictionary::withCapacity(1);
    if (!dict)
        return nullptr;
    const UInt64 wire = OSSwapHostToBigInt64(ti.sas_address);
    OSData *addr = OSData::withBytes(&wire, sizeof(wire));
    if (addr) {
        dict->setObject(kIOPropertySASAddressKey, addr);
        addr->release();
    }
    Klog(MPSK_MAPPING | MPSK_INFO, "target %u: SAS address 0x%016llx, device info 0x%08x%s", (unsigned)targetID,
         (unsigned long long)ti.sas_address, ti.device_info,
         (ti.device_info & MPSK_DEVINFO_SATA_DEVICE) ? " (SATA)" : "");
    return dict;
}

//-----------------------------------------------------------------------------
//	InitializeTargetForID - called from the new nub's start(), before it is
//	registered: refuse IDs the core does not have, give the nub its
//	interconnect characteristics.									[PROTECTED]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::InitializeTargetForID(SCSITargetIdentifier targetID)
{
    if (targetID >= kMaxTargets || !Enter())
        return false;
    bool present = mps_kext_target_present(fMps, (uint32_t)targetID) != 0;
    Leave();
    if (present)
        PublishTargetCharacteristics(targetID);
    return present;
}

//-----------------------------------------------------------------------------
//	PublishTargetCharacteristics - "Physical Interconnect" = SAS (the link
//	between HBA and disk is SAS also for SATA disks, through STP),
//	"Physical Interconnect Location" and the read/write timeouts copied from
//	the controller's personality, merged into the nub's Protocol
//	Characteristics (the family's SetTargetProperty takes only FC/SAS
//	address keys).													[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::PublishTargetCharacteristics(SCSITargetIdentifier targetID)
{
    IOService *nub = (IOService *)GetTargetForID(targetID);   /* IOSCSIParallelInterfaceDevice */
    if (!nub)
        return;

    OSDictionary *dict = nullptr;
    OSObject *obj = nub->copyProperty(kIOPropertyProtocolCharacteristicsKey);
    OSDictionary *old = OSDynamicCast(OSDictionary, obj);
    if (old) {
        OSCollection *copy = old->copyCollection();
        dict = OSDynamicCast(OSDictionary, copy);
        if (!dict)
            OSSafeReleaseNULL(copy);
    }
    OSSafeReleaseNULL(obj);
    if (!dict)
        dict = OSDictionary::withCapacity(4);
    if (!dict)
        return;

    OSString *type = OSString::withCString(kIOPropertyPhysicalInterconnectTypeSerialAttachedSCSI);
    if (type) {
        dict->setObject(kIOPropertyPhysicalInterconnectTypeKey, type);
        type->release();
    }
    OSString *where = OSDynamicCast(OSString, getProperty(kIOPropertyPhysicalInterconnectLocationKey));
    if (where)
        dict->setObject(kIOPropertyPhysicalInterconnectLocationKey, where);
    else if ((where = OSString::withCString(kIOPropertyExternalKey)) != nullptr) {
        dict->setObject(kIOPropertyPhysicalInterconnectLocationKey, where);
        where->release();
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty(kIOPropertyReadTimeOutDurationKey)))
        dict->setObject(kIOPropertyReadTimeOutDurationKey, n);
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty(kIOPropertyWriteTimeOutDurationKey)))
        dict->setObject(kIOPropertyWriteTimeOutDurationKey, n);
    nub->setProperty(kIOPropertyProtocolCharacteristicsKey, dict);
    dict->release();
}

#if 0
#pragma mark -
#pragma mark Termination
#endif

//-----------------------------------------------------------------------------
//	TerminateTargets - after IOSCSIParallelInterfaceController::willTerminate:
//	its loop destroys IDs 0 .. fHighestSupportedDeviceID - 1 only, so destroy
//	ID 63 ourselves (DestroyTargetForID of a missing ID is a no-op).
//																	[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::TerminateTargets(void)
{
    uint64_t left = 0;

    if (fLock) {
        IOLockLock(fLock);
        left = fTargets.Leftovers((uint32_t)ReportHighestSupportedDeviceID());
        IOLockUnlock(fLock);
    }
    for (uint32_t i = 0; i < kMaxTargets; i++) {
        if (left & (1ull << i)) {
            DestroyTargetForID(i);
            Klog(MPSK_MAPPING | MPSK_INFO, "target %u destroyed (missed by the family's willTerminate)", i);
        }
    }
}
