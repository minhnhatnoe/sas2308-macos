/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
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
#include <IOKit/storage/IOBlockStorageDevice.h>
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

//-----------------------------------------------------------------------------
//	TargetUse - is the disk behind targetID in use (S2)? In use: one of its
//	IOMedia objects (whole disk, partitions, anything stacked on them) is
//	open by a client other than a partition scheme, i.e. by its
//	IOMediaBSDClient (a mounted volume opens /dev/diskNsM, a raw reader
//	/dev/(r)diskN; dkopen opens the media with the BSD client as the
//	IOService client) or by some other IOKit client. The partition scheme
//	keeps the whole-disk media open while it is attached: that open does not
//	count. One snapshot of our IORegistry subtree (iterateAll retains every
//	entry, so a disk going away meanwhile is safe), then IOService::isOpen
//	per (media, child) pair. Returns -1: no nub for the ID, 0: not in use,
//	1: in use; *who says why.
//	XNU: no FreeBSD counterpart (FreeBSD never stops an idle disk); the
//	gate check: IOService::isOpen takes the IOService notification lock,
//	which an opener holds across IOMedia::handleOpen -> the block storage
//	driver's handleOpen; we never wait for it with the work-loop gate held
//	(SAM's resend queue can call ProcessParallelTask from inside the gate):
//	then the disk counts as in use, the stop is completed here.	[PRIVATE]
//-----------------------------------------------------------------------------

static bool SAS2308IsBelow(IORegistryEntry *e, IORegistryEntry *top, IORegistryEntry *stop)
{
    for (IORegistryEntry *p = e; p != nullptr && p != stop; p = p->getParentEntry(gIOServicePlane))
        if (p == top)
            return true;
    return false;
}

/*
 * One snapshot of our IORegistry subtree (every entry retained) and the
 * target's nub in it: the IOSCSIParallelInterfaceDevice child whose
 * Protocol Characteristics "SCSI Target Identifier" is targetID. NULL
 * snapshot: no memory; NULL *nub: no nub for the ID.
 */
OSOrderedSet *
SAS2308HBA::TargetSnapshot(uint32_t targetID, IORegistryEntry **nub)
{
    *nub = nullptr;
    IORegistryIterator *it = IORegistryIterator::iterateOver(this, gIOServicePlane, kIORegistryIterateRecursively);
    OSOrderedSet *all = it ? it->iterateAll() : nullptr;
    OSSafeReleaseNULL(it);
    OSObject *o;
    for (unsigned i = 0; all && *nub == nullptr && (o = all->getObject(i)) != nullptr; i++) {
        IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
        if (!e || !e->metaCast("IOSCSIParallelInterfaceDevice") || e->getParentEntry(gIOServicePlane) != this)
            continue;
        OSObject *pc = e->copyProperty(kIOPropertyProtocolCharacteristicsKey);
        OSDictionary *d = OSDynamicCast(OSDictionary, pc);
        OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject(kIOPropertySCSITargetIdentifierKey)) : nullptr;
        if (n && n->unsigned32BitValue() == targetID)
            *nub = e;
        OSSafeReleaseNULL(pc);
    }
    return all;
}

int
SAS2308HBA::TargetUse(uint32_t targetID, char *who, size_t whoLen)
{
    snprintf(who, whoLen, "not open");
    IOWorkLoop *wl = GetWorkLoop();
    if (wl && wl->inGate()) {
        snprintf(who, whoLen, "not checked: work-loop gate held");
        return 1;
    }
    IORegistryEntry *nub;
    OSOrderedSet *all = TargetSnapshot(targetID, &nub);
    if (!all) {
        snprintf(who, whoLen, "not checked: no memory");
        return 1;
    }
    if (!nub) {
        all->release();
        snprintf(who, whoLen, "no nub");
        return -1;
    }
    int use = 0;
    OSObject *o;
    IORegistryEntry *e, *c;
    for (unsigned i = 0; !use && (o = all->getObject(i)) != nullptr; i++) {
        e = OSDynamicCast(IORegistryEntry, o);
        IOService *media = e && e->metaCast("IOMedia") ? OSDynamicCast(IOService, e) : nullptr;
        if (!media || !SAS2308IsBelow(media, nub, this))
            continue;
        for (unsigned j = 0; !use && (o = all->getObject(j)) != nullptr; j++) {
            c = OSDynamicCast(IORegistryEntry, o);
            IOService *client = c ? OSDynamicCast(IOService, c) : nullptr;
            if (!client || client->getParentEntry(gIOServicePlane) != media || client->metaCast("IOPartitionScheme"))
                continue;
            if (media->isOpen(client)) {
                OSObject *bsd = media->copyProperty("BSD Name");
                OSString *name = OSDynamicCast(OSString, bsd);
                snprintf(who, whoLen, "%s open by %s", name ? name->getCStringNoCopy() : "media",
                         client->getMetaClass()->getClassName());
                OSSafeReleaseNULL(bsd);
                use = 1;
            }
        }
    }
    all->release();
    return use;
}

//-----------------------------------------------------------------------------
//	TargetEjected - is SAM's idle-type stop (START 0, LOEJ 0, POWER
//	CONDITION 0) for the disk behind targetID the stop of an eject (EJ)?
//	Asked in sas2308_idle_spindown 0 and 2: an eject's stop is sent, every
//	other one follows the mode.
//	XNU: what `diskutil eject` does to these disks, from Apple's sources and
//	seen on the Mac (2026-11-22, builds 95E0D6C9 and 2CCCBE79):
//	diskarbitrationd unmounts the volumes and issues DKIOCEJECT on the
//	whole disk, which opens the IOBlockStorageDriver with the disk's
//	IOMediaBSDClient, calls ejectMedia() and closes the driver after it
//	returns (IOMediaBSDClient.cpp, DKIOCEJECT). ejectMedia() of a fixed
//	(not removable) disk keeps the media objects: SYNCHRONIZE CACHE, then
//	doEjectMedia (IOBlockStorageDriver.cpp ejectMedia, _ejectable false):
//	SAM's EjectTheMedium calls changePowerStateToPriv(kSBCPowerStateSleep)
//	(IOSCSIBlockCommandsDevice.cpp:181-237), and doEjectMedia then clears
//	IOBlockStorageServices' fMediaPresent (IOBlockStorageServices.cpp:294).
//	PM takes SAM from Active (4) to Sleep (1) in one change, whose
//	HandlePowerChange sends START STOP UNIT IMMED 1, START 0
//	(IOSCSIBlockCommandsDevicePM.cpp:480-489): the same CDB as the idle
//	timer's. Seen: the stop in the same millisecond as the SYNCHRONIZE
//	CACHE and after DKIOCEJECT had closed the driver, SAM's state 4 -> 1,
//	the IOMedia objects kept, `diskutil mount` brings the volume back.
//	The idle timer lowers SAM's state one step per period
//	(IOSCSIPrimaryCommandsDevice.cpp:655-665, numStateTransitions; seen
//	4, 3, 2, 1 every 20-40 s with disksleep 1, S1), Idle (3) sends
//	nothing, so its stops come while SAM is at Idle or Standby;
//	changePowerStateToPriv is called nowhere else in SAM. So an eject's
//	stop is one that arrives while SAM's power state (PM changes it after
//	the driver acknowledges, IOService::getPowerState) is still Active and
//	  - IOBlockStorageServices reports no media (doEjectMedia returned), or
//	  - the IOBlockStorageDriver is open by a client that is not its
//	    whole-disk IOMedia (DKIOCEJECT still in ejectMedia; with the first
//	    test this covers the stop arriving before or after doEjectMedia
//	    returns).
//	fMediaPresent alone is not enough: only a kIOMessageMediaStateHasChanged
//	online sets it again, which SAM sends for new removable media
//	(IOBlockStorageServices.cpp:132-145), so for a fixed disk it stays
//	"no media" after `diskutil mount`; the Active test keeps the later idle
//	stops of such a disk out. A disk with no active whole-disk IOMedia
//	under its driver (removable media, torn down by decommissionMedia) is
//	ejected too. Not used: SAM's "Power Off" property (with it,
//	EjectTheMedium of the open sources sends the stop itself from the
//	ejecting thread; set on all 8 disks with build 2CCCBE79, the eject's
//	stop still came from PM: no 500 ms pause, the driver already closed).
//	Gate held: not ejected (see TargetUse). Returns 1 ejected, 0 not, -1 no
//	nub; *who says why.												[PRIVATE]
//-----------------------------------------------------------------------------

int
SAS2308HBA::TargetEjected(uint32_t targetID, char *who, size_t whoLen)
{
    snprintf(who, whoLen, "not ejected");
    IOWorkLoop *wl = GetWorkLoop();
    if (wl && wl->inGate()) {
        snprintf(who, whoLen, "not checked: work-loop gate held");
        return 0;
    }
    IORegistryEntry *nub;
    OSOrderedSet *all = TargetSnapshot(targetID, &nub);
    if (!all) {
        snprintf(who, whoLen, "not checked: no memory");
        return 0;
    }
    if (!nub) {
        all->release();
        snprintf(who, whoLen, "no nub");
        return -1;
    }
    OSObject *o;
    IOService *driver = nullptr, *media = nullptr;
    for (unsigned i = 0; driver == nullptr && (o = all->getObject(i)) != nullptr; i++) {
        IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
        if (e && e->metaCast("IOBlockStorageDriver") && SAS2308IsBelow(e, nub, this))
            driver = OSDynamicCast(IOService, e);
    }
    IOBlockStorageDevice *dev = driver ? OSDynamicCast(IOBlockStorageDevice, driver->getProvider()) : nullptr;
    IOService *sam = dev ? dev->getProvider() : nullptr;
    if (!driver || !sam) {                  /* SAM's stack not up (yet): not an eject */
        all->release();
        snprintf(who, whoLen, "no block storage driver");
        return 0;
    }
    for (unsigned i = 0; media == nullptr && (o = all->getObject(i)) != nullptr; i++) {
        IOService *s = OSDynamicCast(IOService, o);
        if (s && s->metaCast("IOMedia") && s->getParentEntry(gIOServicePlane) == driver && !s->isInactive())
            media = s;
    }
    int ejected = 0;
    const unsigned long state = sam->getPowerState();
    bool present = true, changed = false;
    if (!media) {
        snprintf(who, whoLen, "no media under the block storage driver");
        ejected = 1;
    } else if (state != kSAMPowerStateActive) {
        snprintf(who, whoLen, "SAM power state %lu (idle timer)", state);
    } else if (dev->reportMediaState(&present, &changed) == kIOReturnSuccess && !present) {
        snprintf(who, whoLen, "SAM power state %lu, no media reported (doEjectMedia)", state);
        ejected = 1;
    }
    for (unsigned i = 0; !ejected && state == kSAMPowerStateActive && (o = all->getObject(i)) != nullptr; i++) {
        IOService *c = OSDynamicCast(IOService, o);
        if (!c || c == media || c == driver || !SAS2308IsBelow(c, driver, this))
            continue;
        if (driver->isOpen(c)) {
            snprintf(who, whoLen, "SAM power state %lu, block storage driver open by %s (DKIOCEJECT)", state,
                     c->getMetaClass()->getClassName());
            ejected = 1;
        }
    }
    if (!ejected && media && state == kSAMPowerStateActive)
        snprintf(who, whoLen, "SAM power state %lu, media present, driver not open for an eject", state);
    all->release();
    return ejected;
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
