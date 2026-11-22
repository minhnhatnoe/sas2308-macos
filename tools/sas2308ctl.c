/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 *
 * sas2308ctl - change SAS2308HBA logging at runtime and read its counters.
 *
 *   sas2308ctl show                     level, debug mask, sink, PCIe link
 *   sas2308ctl stats                    the SAS2308Statistics dictionary
 *   sudo sas2308ctl level <0-4|error|warn|info|debug|trace>
 *   sudo sas2308ctl mask <list>         FreeBSD mps debug classes: "info,fault,recovery",
 *                                       "+trace", "-xinfo", or a number like 0x13
 *   sudo sas2308ctl iolog on|off        IOLog (dmesg) instead of os_log
 *   sudo sas2308ctl reset-stats
 *   sas2308ctl spindown <diskN|all>     stop the disk(s) now: macOS's eject of the whole
 *                                       disk (DADiskEject, as `diskutil eject`), whose stop
 *                                       the kext passes on (EJ); refused for a disk with a
 *                                       mounted volume; the next access spins it up
 *
 * Changes go through IORegistryEntrySetCFProperty -> SAS2308HBA::setProperties,
 * which requires an administrator (root) caller. They last until the kext
 * unloads; the boot default comes from Info.plist / boot-args (design.md).
 *
 * Build: make -C tools   (Command Line Tools, no Xcode needed)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <time.h>

static void Usage(void)
{
    fprintf(stderr, "usage: sas2308ctl show | stats | level <0-4|name> | mask <list> | iolog on|off | reset-stats\n"
                    "       sas2308ctl reinit | hold <ms> | lose <n> | halt  (debug triggers for hardware tests;\n"
                    "                  halt: restart's timeout caps, the controller stops at the next reinit)\n"
                    "       sas2308ctl set <tunable> <n>  (sas2308_* port tunables, e.g. sas2308_send_ordered 0)\n"
                    "       sas2308ctl spindown <diskN|all>  (eject + stop the disk now; not if a volume is mounted)\n");
    exit(2);
}

static void PrintValue(const char *key, CFTypeRef v)
{
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
        printf("  %-28s %lld\n", key, n);
    } else if (CFGetTypeID(v) == CFStringGetTypeID()) {
        char buf[128];
        CFStringGetCString((CFStringRef)v, buf, sizeof(buf), kCFStringEncodingUTF8);
        printf("  %-28s %s\n", key, buf);
    } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        printf("  %-28s %s\n", key, CFBooleanGetValue((CFBooleanRef)v) ? "true" : "false");
    }
}

static void PrintEntry(const void *k, const void *v, void *ctx)
{
    (void)ctx;
    char key[96];
    if (CFGetTypeID(k) != CFStringGetTypeID() ||
        !CFStringGetCString((CFStringRef)k, key, sizeof(key), kCFStringEncodingUTF8))
        return;
    PrintValue(key, v);
}

static int Set(io_service_t svc, const char *key, CFTypeRef value)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    kern_return_t kr = IORegistryEntrySetCFProperty(svc, k, value);
    CFRelease(k);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "sas2308ctl: setting %s failed: 0x%x%s\n", key, kr,
                kr == kIOReturnNotPrivileged ? " (run as root)" :
                kr == kIOReturnBadArgument ? " (bad value)" : "");
        return 1;
    }
    return 0;
}

/*
 * spindown: the disks of one SAS2308HBA are the whole-disk IOMedia whose
 * provider chain reaches an IOSCSIParallelInterfaceDevice (the target nub,
 * "SCSI Target Identifier" in its Protocol Characteristics) attached to that
 * HBA without passing another IOMedia (an APFS container's synthesized disk
 * is under the physical one).
 */
static int TargetOfMedia(io_service_t hba, io_registry_entry_t media, int *tid)
{
    io_registry_entry_t e = media, p;
    IOObjectRetain(e);
    for (;;) {
        if (IORegistryEntryGetParentEntry(e, kIOServicePlane, &p) != KERN_SUCCESS) {
            IOObjectRelease(e);
            return 0;
        }
        IOObjectRelease(e);
        e = p;
        if (IOObjectConformsTo(e, "IOMedia")) {
            IOObjectRelease(e);
            return 0;
        }
        if (IOObjectConformsTo(e, "IOSCSIParallelInterfaceDevice"))
            break;
    }
    int ok = 0;
    if (IORegistryEntryGetParentEntry(e, kIOServicePlane, &p) == KERN_SUCCESS) {
        ok = IOObjectIsEqualTo(p, hba);
        IOObjectRelease(p);
    }
    CFTypeRef pc = IORegistryEntryCreateCFProperty(e, CFSTR("Protocol Characteristics"), NULL, 0);
    CFTypeRef n = pc && CFGetTypeID(pc) == CFDictionaryGetTypeID() ?
                  CFDictionaryGetValue((CFDictionaryRef)pc, CFSTR("SCSI Target Identifier")) : NULL;
    if (!n || CFGetTypeID(n) != CFNumberGetTypeID() || !CFNumberGetValue((CFNumberRef)n, kCFNumberIntType, tid))
        ok = 0;
    if (pc) CFRelease(pc);
    IOObjectRelease(e);
    return ok;
}

/* a mounted file system whose device is `whole' or below it (partitions, APFS volumes): its mount point */
static int MountedOn(io_registry_entry_t whole, char *where, size_t len)
{
    struct statfs *fs;
    int n = getmntinfo(&fs, MNT_NOWAIT);
    for (int i = 0; i < n; i++) {
        if (strncmp(fs[i].f_mntfromname, "/dev/disk", 9) != 0)
            continue;
        io_registry_entry_t e = IOServiceGetMatchingService(kIOMainPortDefault,
                                    IOBSDNameMatching(kIOMainPortDefault, 0, fs[i].f_mntfromname + 5));
        while (e != IO_OBJECT_NULL) {
            if (IOObjectIsEqualTo(e, whole)) {
                IOObjectRelease(e);
                snprintf(where, len, "%s on %s", fs[i].f_mntfromname + 5, fs[i].f_mntonname);
                return 1;
            }
            io_registry_entry_t p;
            if (IORegistryEntryGetParentEntry(e, kIOServicePlane, &p) != KERN_SUCCESS)
                p = IO_OBJECT_NULL;
            IOObjectRelease(e);
            e = p;
        }
    }
    return 0;
}

static double Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* DADiskEject's answer */
struct EjectResult { int done; DAReturn status; char why[256]; };

static void EjectDone(DADiskRef disk, DADissenterRef dissenter, void *context)
{
    struct EjectResult *r = (struct EjectResult *)context;
    (void)disk;
    r->done = 1;
    r->status = dissenter ? DADissenterGetStatus(dissenter) : kDAReturnSuccess;
    CFStringRef s = dissenter ? DADissenterGetStatusString(dissenter) : NULL;
    if (!s || !CFStringGetCString(s, r->why, sizeof(r->why), kCFStringEncodingUTF8))
        r->why[0] = 0;
}

/*
 * stop `media' (a disk of `hba'); returns 1 on failure or refusal.
 * The stop is macOS's own eject (DADiskEject, what `diskutil eject` does):
 * DKIOCEJECT -> SAM's EjectTheMedium, whose START STOP UNIT the kext passes
 * on for an ejected disk (EJ, design.md). A START STOP UNIT of our own (S2
 * sent camcontrol stop's) leaves the IOC's SAT layer answering every read
 * NOT READY 04/02 until a START, which SAM only sends when it believes the
 * disk stopped (seen on the Mac, 2026-11-22). After the eject SAM does, and
 * the next access (a read, `diskutil mount`) sends START and spins it up.
 */
static int SpinDownOne(io_service_t hba, io_registry_entry_t media, const char *bsd, int tid)
{
    char where[1200];
    (void)hba;
    if (MountedOn(media, where, sizeof(where))) {
        printf("  %s (target %d): not stopped: mounted (%s)\n", bsd, tid, where);
        return 1;
    }
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    DADiskRef disk = session ? DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd) : NULL;
    if (!disk) {
        printf("  %s (target %d): not stopped: DiskArbitration has no such disk\n", bsd, tid);
        if (session) CFRelease(session);
        return 1;
    }
    struct EjectResult r = { 0, 0, "" };
    const double t0 = Now();
    DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    DADiskEject(disk, kDADiskEjectOptionDefault, EjectDone, &r);
    while (!r.done && Now() - t0 < 60)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, true);
    DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRelease(disk);
    CFRelease(session);
    if (r.done && r.status == kDAReturnSuccess) {
        printf("  %s (target %d): ejected, stop sent (%.1f s); `diskutil mount` or a read spins it up\n",
               bsd, tid, Now() - t0);
        return 0;
    }
    printf("  %s (target %d): not stopped: %s (0x%x)%s%s\n", bsd, tid,
           !r.done ? "no answer from DiskArbitration in 60 s" :
           r.status == kDAReturnBusy ? "in use (a volume mounted or a /dev node open)" :
           r.status == kDAReturnNotPrivileged ? "not permitted" : "eject failed",
           r.status, r.why[0] ? ": " : "", r.why);
    return 1;
}

/* sas2308ctl spindown <diskN|all> for one HBA; *seen counts the disks matched */
static int SpinDown(io_service_t hba, const char *arg, int *seen)
{
    if (!strncmp(arg, "/dev/", 5))
        arg += 5;
    if (!strncmp(arg, "rdisk", 5))
        arg++;
    io_iterator_t it;
    if (IORegistryEntryCreateIterator(hba, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS)
        return 1;
    int rc = 0;
    io_registry_entry_t e;
    while ((e = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        char bsd[64] = "";
        CFTypeRef name = IORegistryEntryCreateCFProperty(e, CFSTR("BSD Name"), NULL, 0);
        CFTypeRef whole = IORegistryEntryCreateCFProperty(e, CFSTR("Whole"), NULL, 0);
        int tid = -1;
        if (IOObjectConformsTo(e, "IOMedia") && name && CFGetTypeID(name) == CFStringGetTypeID() &&
            CFStringGetCString((CFStringRef)name, bsd, sizeof(bsd), kCFStringEncodingUTF8) &&
            whole == kCFBooleanTrue && (!strcmp(arg, "all") || !strcmp(arg, bsd)) &&
            TargetOfMedia(hba, e, &tid)) {
            (*seen)++;
            rc |= SpinDownOne(hba, e, bsd, tid);
        }
        if (name) CFRelease(name);
        if (whole) CFRelease(whole);
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) Usage();
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("SAS2308HBA"), &it) != KERN_SUCCESS) {
        fprintf(stderr, "sas2308ctl: IOServiceGetMatchingServices failed\n");
        return 1;
    }
    int found = 0, rc = 0, seen = 0;
    io_service_t svc;
    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        found++;
        printf("SAS2308HBA instance %d:\n", found - 1);
        const char *cmd = argv[1];
        if (!strcmp(cmd, "show") || !strcmp(cmd, "stats")) {
            const char *keys[] = { "SAS2308LogLevel", "SAS2308DebugMask", "SAS2308LogToIOLog" };
            for (int i = 0; i < 3 && !strcmp(cmd, "show"); i++) {
                CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
                CFTypeRef v = IORegistryEntryCreateCFProperty(svc, k, NULL, 0);
                CFRelease(k);
                if (v) { PrintValue(keys[i], v); CFRelease(v); }
            }
            if (!strcmp(cmd, "show")) {
                /* the card's own PCIe registers (speed, width, MPS, MRRS), read at attach and after a reinit */
                CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("PCIe Link"), NULL, 0);
                if (v && CFGetTypeID(v) == CFDictionaryGetTypeID()) {
                    printf("  PCIe Link:\n");
                    CFDictionaryApplyFunction((CFDictionaryRef)v, PrintEntry, NULL);
                }
                if (v) CFRelease(v);
            }
            if (!strcmp(cmd, "stats")) {
                CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("SAS2308Statistics"), NULL, 0);
                if (v && CFGetTypeID(v) == CFDictionaryGetTypeID())
                    CFDictionaryApplyFunction((CFDictionaryRef)v, PrintEntry, NULL);
                else
                    printf("  (no statistics published yet)\n");
                if (v) CFRelease(v);
            }
        } else if (!strcmp(cmd, "level") && argc == 3) {
            CFStringRef s = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
            rc |= Set(svc, "SAS2308LogLevel", s);
            CFRelease(s);
        } else if (!strcmp(cmd, "mask") && argc == 3) {
            CFStringRef s = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
            rc |= Set(svc, "SAS2308DebugMask", s);
            CFRelease(s);
        } else if (!strcmp(cmd, "iolog") && argc == 3) {
            rc |= Set(svc, "SAS2308LogToIOLog", !strcmp(argv[2], "on") ? kCFBooleanTrue : kCFBooleanFalse);
        } else if (((!strcmp(cmd, "reinit") || !strcmp(cmd, "halt")) && argc == 2) ||
                   ((!strcmp(cmd, "hold") || !strcmp(cmd, "lose")) && argc == 3)) {
            const char *key = !strcmp(cmd, "reinit") ? "SAS2308DebugReinit" :
                              !strcmp(cmd, "halt") ? "SAS2308DebugHalt" :
                              !strcmp(cmd, "hold") ? "SAS2308DebugHoldMs" : "SAS2308DebugLose";
            int v = argc == 3 ? atoi(argv[2]) : 1;
            CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &v);
            rc |= Set(svc, key, num);
            CFRelease(num);
        } else if (!strcmp(cmd, "set") && argc == 4 && !strncmp(argv[2], "sas2308_", 8)) {
            int v = atoi(argv[3]);
            CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &v);
            rc |= Set(svc, argv[2], num);
            CFRelease(num);
        } else if (!strcmp(cmd, "spindown") && argc == 3) {
            rc |= SpinDown(svc, argv[2], &seen);
        } else if (!strcmp(cmd, "reset-stats")) {
            rc |= Set(svc, "SAS2308ResetStats", kCFBooleanTrue);
        } else {
            Usage();
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    if (found && !strcmp(argv[1], "spindown") && argc == 3 && !seen) {
        fprintf(stderr, "sas2308ctl: %s is not a whole disk behind SAS2308HBA\n", argv[2]);
        return 1;
    }
    if (!found) {
        fprintf(stderr, "sas2308ctl: no SAS2308HBA instance in the IORegistry (kext not loaded or card absent)\n");
        return 1;
    }
    return rc;
}
