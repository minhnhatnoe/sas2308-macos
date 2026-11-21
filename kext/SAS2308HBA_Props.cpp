/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * SAS2308HBA_Props.cpp - registry properties: logging control (personality,
 * boot-args, setProperties from sas2308ctl), the controller characteristics
 * the storage stack shows (System Information), and the statistics
 * dictionary (SAS2308Statistics), published at most once a second.
 */
#include "SAS2308HBA.h"
#include "SAS2308HBA_PCIe.h"

#include <IOKit/IOUserClient.h>
#include <IOKit/storage/IOStorageControllerCharacteristics.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

#ifndef SAS2308_BUNDLE_ID
#define SAS2308_BUNDLE_ID "org.sas2308.driver.SAS2308HBA"
#endif

/* MPI2_SCSITASKMGMT_TASKTYPE_* (core/mpi/mpi2_init.h) */
enum : uint8_t {
    kTmAbortTask    = 0x01,
    kTmAbortTaskSet = 0x02,
    kTmTargetReset  = 0x03,
    kTmLunReset     = 0x05,
    kTmClearTaskSet = 0x06,
    kTmClearAca     = 0x08,
};

static uint64_t
UptimeUs(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

#if 0
#pragma mark -
#pragma mark Logging control
#endif

//-----------------------------------------------------------------------------
//	Klog - one line through mps_kext_log() (os_log per class, or IOLog).
//																	[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::Klog(uint32_t cls, const char *fmt, ...)
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

/* Whatever the mask: the operator's own actions (level change, debug triggers). */
void
SAS2308HBA::KlogAlways(uint32_t cls, const char *fmt, ...)
{
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
void
SAS2308HBA::ApplyLogSettings(void)
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
    if (fGlue.fMps)
        mps_kext_set_debug(fGlue.fMps, mask);
    setProperty(kLogLevelKey, (unsigned long long)level, 32);
    setProperty(kDebugMaskKey, (unsigned long long)mask, 32);
    char names[128];
    mps_kext_describe_debug(mask, names, sizeof(names));
    Klog(MPSK_INIT | MPSK_INFO, "log level %d (%s), debug mask 0x%x (%s), sink %s", level,
         mps_kext_level_name(level), mask, names, fHost.use_iolog ? "IOLog" : "os_log subsystem " SAS2308_BUNDLE_ID);
}

/* sas2308ctl / IORegistryEntrySetCFProperties(): change the level at runtime. Admin only. */
IOReturn
SAS2308HBA::setProperties(OSObject *properties)
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
        if (fGlue.fMps)
            mps_kext_set_debug(fGlue.fMps, mask);
        if (level >= 0)
            setProperty(kLogLevelKey, (unsigned long long)level, 32);
        setProperty(kDebugMaskKey, (unsigned long long)mask, 32);
        char names[128];
        mps_kext_describe_debug(mask, names, sizeof(names));
        /* info (persisted), and past the mask: visible at every level (D23) */
        KlogAlways(MPSK_INFO, "debug mask 0x%x -> 0x%x (%s), set from userland", old, mask, names);
    }
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject(kIOLogKey))) {
        fHost.use_iolog = b->isTrue();
        setProperty(kIOLogKey, b->isTrue());
        handled = true;
    }
    static const struct { const char *key; int what; } dbg[] = {
        { "SAS2308DebugReinit", MPSK_DEBUG_REINIT },
        { "SAS2308DebugHoldMs", MPSK_DEBUG_HOLD },
        { "SAS2308DebugLose", MPSK_DEBUG_LOSE },
    };
    for (const auto &e : dbg) {
        OSNumber *n = OSDynamicCast(OSNumber, d->getObject(e.key));
        if (!n)
            continue;
        int rc = -1;
        if (Enter()) {
            rc = mps_kext_debug(fGlue.fMps, e.what, n->unsigned32BitValue());
            Leave();
        }
        KlogAlways(MPSK_INFO, "debug trigger %s %u from userland: %s", e.key, n->unsigned32BitValue(),
                   rc == 0 ? "done" : "refused");
        if (rc != 0)
            return kIOReturnBadArgument;
        handled = true;
    }
    if (d->getObject(kResetStatsKey)) {
        if (Enter()) {
            mps_kext_reset_stats(fGlue.fMps);
            Leave();
        }
        if (fGlue.HasLock()) {
            fGlue.Lock();
            fGlue.fKStats = KStats();
            fGlue.Unlock();
        }
        Klog(MPSK_INFO, "statistics reset from userland");
        PublishStats(true);
        handled = true;
    }
    return handled ? kIOReturnSuccess : kIOReturnUnsupported;
}

#if 0
#pragma mark -
#pragma mark Controller characteristics
#endif

//-----------------------------------------------------------------------------
//	PublishControllerCharacteristics - what System Information and the
//	storage stack show for the HBA ("Controller Characteristics", through
//	the family's SetHBAProperty): vendor, product, firmware revision, port
//	speed and description.											[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::PublishControllerCharacteristics(void)
{
    struct mps_kext_controller_info ci;

    if (!Enter())
        return;
    mps_kext_controller_info(fGlue.fMps, &ci);
    Leave();

    const UInt16 subVendor = fPCI ? fPCI->configRead16(kIOPCIConfigSubSystemVendorID) : 0;
    const UInt16 subDevice = fPCI ? fPCI->configRead16(kIOPCIConfigSubSystemID) : 0;
    /* LSI subsystem IDs (Linux mpt3sas / LSI release notes): 0x3020 = SAS9207-8i, 0x3040 = SAS9207-8e */
    const char *product = (subVendor == 0x1000 && subDevice == 0x3020) ? "SAS9207-8i" :
                          (subVendor == 0x1000 && subDevice == 0x3040) ? "SAS9207-8e" : "SAS2308";
    char desc[96];
    snprintf(desc, sizeof(desc), "%u-port SAS2 HBA, MPI %s%s", ci.num_ports ? ci.num_ports : 8,
             ci.msg_version[0] ? ci.msg_version : "2.0", fTunnelled ? ", Thunderbolt" : "");

    struct { const char *key; const char *value; } props[] = {
        { kIOPropertyVendorNameKey,           "LSI" },
        { kIOPropertyProductNameKey,          product },
        { kIOPropertyProductRevisionLevelKey, ci.fw_version[0] ? ci.fw_version : "unknown" },
        { kIOPropertyPortSpeedKey,            kIOPropertyPortSpeed6GigabitKey },   /* SAS2: 6 Gb/s per PHY */
        { kIOPropertyPortDescriptionKey,      desc },
    };
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        OSString *s = OSString::withCString(props[i].value);
        if (!s)
            continue;
        if (!SetHBAProperty(props[i].key, s))
            Klog(MPSK_ERROR | MPSK_INIT, "SetHBAProperty(%s) refused", props[i].key);
        s->release();
    }
    Klog(MPSK_INIT | MPSK_INFO, "controller: LSI %s, firmware %s, MPI %s, product id 0x%04x", product,
         ci.fw_version, ci.msg_version, ci.product_id);
}

//-----------------------------------------------------------------------------
//	PublishLimits - "Controller Limits": the IOCFacts limits and the queue
//	sizes mps_resize_queues derived from them, plus the memory they cost,
//	and "PCIe Link". At attach (logged once) and again after every IOC
//	reinit or resume (PublishStats), since the facts are re-read then.
//																	[PRIVATE]
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//	PublishPcieLink - "PCIe Link": the card's own PCI Express registers: the
//	negotiated speed and width (Link Status) against what the card can do
//	(Link Capabilities), the Max Payload Size and Max Read Request Size in
//	use (Device Control) and the largest payload it supports (Device
//	Capabilities). Not when the card is gone (all-ones) or asleep. With
//	"Controller Limits": at attach and after every reinit or resume.
//																	[PRIVATE]
//-----------------------------------------------------------------------------

void
SAS2308HBA::PublishPcieLink(bool log)
{
    UInt8 cap = 0;

    if (!fPCI || fSuspended || fPCI->configRead16(kIOPCIConfigVendorID) != 0x1000 ||
        !fPCI->findPCICapability(kIOPCIPCIExpressCapability, &cap) || cap == 0)
        return;
    const uint32_t devCap = fPCI->configRead32(cap + SAS2308Pcie::kDevCap);
    const uint16_t devCtl = fPCI->configRead16(cap + SAS2308Pcie::kDevCtl);
    const uint32_t linkCap = fPCI->configRead32(cap + SAS2308Pcie::kLinkCap);
    const uint16_t linkSta = fPCI->configRead16(cap + SAS2308Pcie::kLinkSta);
    if (devCap == 0xFFFFFFFFu || linkCap == 0xFFFFFFFFu)
        return;
    const SAS2308Pcie::Link k = SAS2308Pcie::Decode(devCap, devCtl, linkCap, linkSta);
    const char *speed = SAS2308Pcie::SpeedName(k.speed);
    const char *maxSpeed = SAS2308Pcie::SpeedName(k.maxSpeed);

    OSDictionary *d = OSDictionary::withCapacity(12);
    if (!d)
        return;
    OSString *str = OSString::withCString(speed ? speed : "unknown");
    if (str) {
        d->setObject("Link Speed", str);
        str->release();
    }
    SetNumber(d, "Link Width", k.width);
    str = OSString::withCString(maxSpeed ? maxSpeed : "unknown");
    if (str) {
        d->setObject("Max Link Speed", str);
        str->release();
    }
    SetNumber(d, "Max Link Width", k.maxWidth);
    SetNumber(d, "Max Payload Size (bytes)", k.mps);
    SetNumber(d, "Max Payload Size Supported (bytes)", k.mpsSupported);
    SetNumber(d, "Max Read Request Size (bytes)", k.mrrs);
    SetNumber(d, "Device Capabilities", devCap);
    SetNumber(d, "Device Control", devCtl);
    SetNumber(d, "Link Capabilities", linkCap);
    SetNumber(d, "Link Status", linkSta);
    d->setObject("BAR1 Posted Writes", fPostedWrite ? kOSBooleanTrue : kOSBooleanFalse);   /* D75 */
    setProperty(kPcieLinkKey, d);
    d->release();
    if (log)
        Klog(MPSK_INIT | MPSK_INFO, "PCIe link %s x%u (card: %s x%u), Max Payload Size %u (supported %u), "
             "Max Read Request Size %u bytes", speed ? speed : "unknown", k.width, maxSpeed ? maxSpeed : "unknown",
             k.maxWidth, k.mps, k.mpsSupported, k.mrrs);
}

void
SAS2308HBA::PublishLimits(bool log)
{
    struct mps_kext_limits l;

    PublishPcieLink(log);

    if (!Enter())
        return;
    const int rc = mps_kext_limits(fGlue.fMps, &l);
    Leave();
    if (rc != 0)
        return;
    fGlue.Lock();
    const uint32_t tasks = fGlue.fMaxTasks;
    fGlue.Unlock();
    const uint64_t taskDataBytes = (uint64_t)tasks * mps_kext_task_size(fGlue.fMps);

    OSDictionary *d = OSDictionary::withCapacity(24);
    if (!d)
        return;
    /* IOCFacts */
    SetNumber(d, "RequestCredit", l.request_credit);
    SetNumber(d, "HighPriorityCredit", l.high_priority_credit);
    SetNumber(d, "MaxReplyDescriptorPostQueueDepth", l.max_reply_post_depth);
    SetNumber(d, "MaxChainDepth", l.max_chain_depth);
    SetNumber(d, "IOCRequestFrameSize (bytes)", l.req_frame_bytes);
    SetNumber(d, "ReplyFrameSize (bytes)", l.reply_frame_bytes);
    SetNumber(d, "MaxTargets", l.max_targets);
    SetNumber(d, "MaxVolumes", l.max_volumes);
    SetNumber(d, "MaxMSIxVectors", l.max_msix_vectors);
    /* effective */
    SetNumber(d, "Request frames", l.req_frames);
    SetNumber(d, "High-priority frames", l.pri_req_frames);
    SetNumber(d, "Reply frames", l.reply_frames);
    SetNumber(d, "Reply post queue depth", l.post_queue_depth);
    SetNumber(d, "Chain frames", l.chain_frames);
    SetNumber(d, "Max segments per IO", l.max_segs);
    SetNumber(d, "Core OS commands", l.os_commands);
    SetNumber(d, "OS task count", tasks);
    SetNumber(d, "DMA bytes", l.dma_bytes);
    SetNumber(d, "Heap bytes", l.heap_bytes);
    SetNumber(d, "Task data bytes", taskDataBytes);
    setProperty(kLimitsKey, d);
    d->release();

    if (log)
        Klog(MPSK_INIT | MPSK_INFO, "IOCFacts: RequestCredit %u, HighPriorityCredit %u, MaxReplyDescriptorPostQueueDepth %u, "
             "MaxChainDepth %u, request frame %u bytes, MaxTargets %u; using %u request + %u high-priority frames, "
             "%u replies, %u chain frames, %u OS tasks; memory: DMA %llu, heap %llu, task data %llu bytes",
             l.request_credit, l.high_priority_credit, l.max_reply_post_depth, l.max_chain_depth, l.req_frame_bytes,
             l.max_targets, l.req_frames, l.pri_req_frames, l.reply_frames, l.chain_frames, tasks,
             (unsigned long long)l.dma_bytes, (unsigned long long)l.heap_bytes,
             (unsigned long long)taskDataBytes);
}

#if 0
#pragma mark -
#pragma mark Statistics
#endif

void
SAS2308HBA::SetNumber(OSDictionary *dict, const char *key, UInt64 value)
{
    OSNumber *n = OSNumber::withNumber(value, 64);
    if (n) {
        dict->setObject(key, n);
        n->release();
    }
}

static uint32_t TmSentTotal(const struct mps_xnu_stats &st)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 16; i++) n += st.tm_sent[i];
    return n;
}

/* The core's counters: under mps_mtx while attached, as they were left after mps_free. */
bool
SAS2308HBA::Snapshot(struct mps_kext_snapshot *snap)
{
    if (Enter()) {
        mps_kext_snapshot(fGlue.fMps, snap);
        Leave();
        return true;
    }
    if (!fGlue.HasLock() || !fGlue.fMps)
        return false;
    fGlue.Lock();
    const bool detached = !fGlue.fCoreAttached && !fGlue.fBusy;
    fGlue.Unlock();
    if (!detached)
        return false;
    mps_kext_snapshot(fGlue.fMps, snap);
    return true;
}

/*
 * Copy the counters (struct copies), build the dictionary outside every
 * lock. The IO path only increments plain integers; nothing is published
 * per IO.
 */
void
SAS2308HBA::PublishStats(bool force)
{
    if (!fGlue.HasLock())
        return;
    const uint64_t now = UptimeUs();
    fGlue.Lock();
    if (!force && now - fLastPublishUs < (uint64_t)kStatsPublishMs * 1000) {
        fGlue.Unlock();
        return;
    }
    const KStats ks = fGlue.fKStats;
    fGlue.Unlock();

    struct mps_kext_snapshot snap;
    if (!Snapshot(&snap))
        return;
    const struct mps_xnu_stats &st = snap.st;
    const bool gone = snap.gone, resetting = snap.resetting;
    const uint32_t mask = fDebugMask;
    const uint64_t limitsSig = (uint64_t)st.reinits + st.reinit_failures + st.resumes;
    const uint64_t sig = st.ios + st.lat_sum_us + st.held + st.io_timeouts + TmSentTotal(st) + st.tm_timeouts +
                         st.faults + st.reinits + st.reinit_failures + st.stale_replies + st.bad_replies + st.events +
                         st.chain_alloc_fail + ks.tasks + ks.tasksRejected + ks.familyTimeouts + ks.tmRequests +
                         snap.held + snap.tms_busy + (gone ? 1 : 0) + (resetting ? 2 : 0) + (fSuspended ? 4 : 0) + mask +
                         st.suspends + st.resumes + st.config_reads + st.config_retries + st.readd_scheduled +
                         st.recovered_sense + st.temp_reads + (uint32_t)st.ioc_temp_c;
    fGlue.Lock();
    if (!force && sig == fLastPublishedSig) {
        fGlue.Unlock();
        return;
    }
    fLastPublishUs = now;
    fLastPublishedSig = sig;
    fGlue.Unlock();

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
    SetNumber(d, "Family timer re-arms", st.timer_rearms);
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
    SetNumber(d, "Lost completion kicks", ks.lostKicks);
    SetNumber(d, "Tasks waiting for a mapping", ks.mapWaits);  /* IOVA shortage: mapped again later (D47) */
    SetNumber(d, "Transient all-ones reads", st.transient_allones);
    {
        uint16_t cmd, ctl;
        uint64_t addr;
        uint32_t data;
        ReadMsi(&cmd, &ctl, &addr, &data);          /* D13: compare before / after a diag reset */
        SetNumber(d, "PCI command", cmd);
        SetNumber(d, "MSI control", ctl);
        SetNumber(d, "MSI address", addr);
        SetNumber(d, "MSI data", data);
        SetNumber(d, "PCI state restores", fPciRestores);
        SetNumber(d, "PCI state restore failures", fPciRestoreFailures);
    }
    SetNumber(d, "Stale replies", st.stale_replies);
    SetNumber(d, "Bad replies", st.bad_replies);
    SetNumber(d, "Events", st.events);
    SetNumber(d, "Event acks sent", st.event_acks);     /* decides D29 (FreeBSD never acks) */
    SetNumber(d, "Event acks dropped", st.event_ack_dropped);
    SetNumber(d, "Chain alloc failures", st.chain_alloc_fail);
    SetNumber(d, "Chain free low", st.chain_free_low);
    SetNumber(d, "Suspends", st.suspends);
    SetNumber(d, "Resumes", st.resumes);
    SetNumber(d, "Resumes without reset", st.resume_ready);
    SetNumber(d, "Resumes with diag reset", st.resume_diag_reset);
    SetNumber(d, "Suspend drain left", st.suspend_drain_left);
    SetNumber(d, "Config page reads", st.config_reads);
    SetNumber(d, "Config page failures", st.config_failures);
    SetNumber(d, "Config page retries", st.config_retries);
    SetNumber(d, "Device re-adds scheduled", st.readd_scheduled);
    SetNumber(d, "Device re-adds given up", st.readd_given_up);
    SetNumber(d, "Recovered sense (not errors)", st.recovered_sense);
    SetNumber(d, "Unit attentions after reset retried", st.ua_retries);
    SetNumber(d, "IOC temperature reads", st.temp_reads);
    SetNumber(d, "IOC temperature read failures", st.temp_failures);
    SetNumber(d, "IOC temperature read timeouts", st.temp_timeouts);   /* polling stops until a reinit (D31) */
    SetNumber(d, "IOC temperature warnings", st.temp_warnings);
    if (st.ioc_temp_valid && st.ioc_temp_c >= 0) {
        SetNumber(d, "IOC temperature (C)", (UInt64)st.ioc_temp_c);
        /* also a top-level property: ioreg -l -k "IOC Temperature (C)" */
        setProperty(kIocTemperatureKey, (unsigned long long)st.ioc_temp_c, 32);
    }
    SetNumber(d, "KASSERT hits", st.kassert_hits);   /* KASSERT / panic() survived, all controllers (D55) */
    SetNumber(d, "Debug mask", mask);
    OSString *state = OSString::withCString(gone ? "gone" : fSuspended ? "suspended" : resetting ? "resetting" : "operational");
    if (state) {
        d->setObject("State", state);
        state->release();
    }
    setProperty(kStatsKey, d);
    d->release();

    /* the IOCFacts were re-read: refresh "Controller Limits" */
    fGlue.Lock();
    const bool limits = limitsSig != fLimitsSig;
    fLimitsSig = limitsSig;
    fGlue.Unlock();
    if (limits)
        PublishLimits(false);
}

void
SAS2308HBA::LogCounters(const char *why)
{
    if (!fGlue.HasLock())
        return;
    struct mps_kext_snapshot snap;
    if (!Snapshot(&snap))
        return;
    fGlue.Lock();
    const KStats ks = fGlue.fKStats;
    fGlue.Unlock();
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

void
SAS2308HBA::StatsTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
    SAS2308HBA *self = OSDynamicCast(SAS2308HBA, owner);
    if (!self || !self->fGlue.HasLock())
        return;
    self->fGlue.Lock();
    const bool run = self->fGlue.fStarted && !self->fGlue.fStopping;
    self->fGlue.Unlock();
    if (!run)
        return;
    self->fGlue.TimerDeliver();                     /* lost kicks (D43), mapping retries (D47) */
    self->PublishStats(false);
    sender->setTimeoutMS(kStatsPublishMs);
}
