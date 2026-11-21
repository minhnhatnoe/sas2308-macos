/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * SAS2308HBA_Hotplug.cpp - SCSI target nubs: created and destroyed from the
 * core's discovery events, their properties, and their teardown when the
 * controller terminates.
 *
 * Target creation runs on its own thread call: CreateTargetForID() probes
 * the device synchronously, and those probe IOs complete on the work loop.
 * The thread's loop and the bookkeeping are IOKit-free (SAS2308Glue.cpp,
 * SAS2308HBA_Targets.h, host-tested); this file has the IOKit parts.
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
//	HostAsync - CAM async events (mps_mtx held): SAS2308Glue::Async updates
//	the target table and kicks the hotplug thread.					[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::HostAsync(void *hba, int event, uint32_t tid, const uint8_t *present, const uint32_t *gens,
                      uint32_t count)
{
    ((SAS2308HBA *)hba)->fGlue.Async(event, tid, present, gens, count);
}

#if 0
#pragma mark -
#pragma mark The hotplug thread
#endif

/*
 * The hotplug taskqueue's task (L10): one kernel thread, as FreeBSD's
 * event taskqueue and CAM's xpt_scanner_thread; CreateTargetForID's
 * synchronous probe and the target drain block only it.
 */
void
SAS2308HBA::HotplugThread(void *self)
{
    ((SAS2308HBA *)self)->fGlue.HotplugCallBody();
}

//-----------------------------------------------------------------------------
//	PortCreateTarget - CreateTargetForID() for the hotplug thread (no lock
//	held): it probes the device synchronously. Never once terminating.
//																	[PRIVATE]
//-----------------------------------------------------------------------------

bool
SAS2308HBA::PortCreateTarget(uint32_t targetID)
{
    if (isInactive()) {
        Klog(MPSK_MAPPING | MPSK_INFO, "target %u: not created, controller terminating", targetID);
        return false;
    }
    OSDictionary *props = CopyTargetProperties(targetID);
    const bool ok = props ? CreateTargetForID(targetID, props) : CreateTargetForID(targetID);
    OSSafeReleaseNULL(props);
    Klog(ok ? (MPSK_MAPPING | MPSK_INFO) : (MPSK_MAPPING | MPSK_INFO | MPSK_ERROR), "CreateTargetForID(%u) %s", targetID,
         ok ? "done" : "FAILED");
    return ok;
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
        found = mps_kext_target_info(fGlue.fMps, (uint32_t)targetID, &ti);
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
    if (targetID >= fGlue.fMaxTargets || !Enter())
        return false;
    bool present = mps_kext_target_present(fGlue.fMps, (uint32_t)targetID) != 0;
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
