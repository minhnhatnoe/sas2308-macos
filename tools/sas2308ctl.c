/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * sas2308ctl - change SAS2308HBA logging at runtime and read its counters.
 *
 *   sas2308ctl show                     level, debug mask, sink
 *   sas2308ctl stats                    the SAS2308Statistics dictionary
 *   sudo sas2308ctl level <0-4|error|warn|info|debug|trace>
 *   sudo sas2308ctl mask <list>         FreeBSD mps debug classes: "info,fault,recovery",
 *                                       "+trace", "-xinfo", or a number like 0x13
 *   sudo sas2308ctl iolog on|off        IOLog (dmesg) instead of os_log
 *   sudo sas2308ctl reset-stats
 *
 * Changes go through IORegistryEntrySetCFProperty -> SAS2308HBA::setProperties,
 * which requires an administrator (root) caller. They last until the kext
 * unloads; the boot default comes from Info.plist / boot-args (design.md).
 *
 * Build: make -C tools   (Command Line Tools, no Xcode needed)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Usage(void)
{
    fprintf(stderr, "usage: sas2308ctl show | stats | level <0-4|name> | mask <list> | iolog on|off | reset-stats\n"
                    "       sas2308ctl reinit | hold <ms> | lose <n>     (debug triggers for hardware tests)\n");
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

int main(int argc, char **argv)
{
    if (argc < 2) Usage();
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("SAS2308HBA"), &it) != KERN_SUCCESS) {
        fprintf(stderr, "sas2308ctl: IOServiceGetMatchingServices failed\n");
        return 1;
    }
    int found = 0, rc = 0;
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
        } else if ((!strcmp(cmd, "reinit") && argc == 2) ||
                   ((!strcmp(cmd, "hold") || !strcmp(cmd, "lose")) && argc == 3)) {
            const char *key = !strcmp(cmd, "reinit") ? "SAS2308DebugReinit" :
                              !strcmp(cmd, "hold") ? "SAS2308DebugHoldMs" : "SAS2308DebugLose";
            int v = argc == 3 ? atoi(argv[2]) : 1;
            CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &v);
            rc |= Set(svc, key, num);
            CFRelease(num);
        } else if (!strcmp(cmd, "reset-stats")) {
            rc |= Set(svc, "SAS2308ResetStats", kCFBooleanTrue);
        } else {
            Usage();
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    if (!found) {
        fprintf(stderr, "sas2308ctl: no SAS2308HBA instance in the IORegistry (kext not loaded or card absent)\n");
        return 1;
    }
    return rc;
}
