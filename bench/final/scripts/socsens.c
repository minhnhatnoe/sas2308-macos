/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 */
/* socsens: print Apple-silicon temperature sensors (IOHIDEventSystem, private API).
 * usage: socsens        -> "name=value" for every temperature sensor
 *        socsens -s     -> one line "tdie_max tdie_avg n_tdie all_max" (C)
 * build: clang -O2 -o socsens socsens.c -framework IOKit -framework CoreFoundation */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <stdio.h>
#include <string.h>
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef);
extern int IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef, CFDictionaryRef);
extern IOHIDEventRef IOHIDServiceClientCopyEvent(IOHIDServiceClientRef, int64_t, int32_t, int64_t);
extern CFStringRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef, CFStringRef);
extern double IOHIDEventGetFloatValue(IOHIDEventRef, int32_t);
int main(int argc, char **argv)
{
    int summary = argc > 1 && !strcmp(argv[1], "-s");
    int page = 0xff00, usage = 5;
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &page), u = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    const void *k[] = { CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage") }, *v[] = { p, u };
    CFDictionaryRef m = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDEventSystemClientRef c = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
    IOHIDEventSystemClientSetMatching(c, m);
    CFArrayRef s = IOHIDEventSystemClientCopyServices(c);
    if (!s) { printf("no sensors\n"); return 1; }
    double tmax = -1, tsum = 0, amax = -1; int tn = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(s); i++) {
        IOHIDServiceClientRef sc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(s, i);
        CFStringRef name = IOHIDServiceClientCopyProperty(sc, CFSTR("Product"));
        IOHIDEventRef e = IOHIDServiceClientCopyEvent(sc, 15, 0, 0);
        if (!e) { if (name) CFRelease(name); continue; }
        double t = IOHIDEventGetFloatValue(e, 15 << 16);
        char nb[128] = "?"; if (name) CFStringGetCString(name, nb, sizeof nb, kCFStringEncodingUTF8);
        if (t > 0 && t < 150 && !strstr(nb, "tcal")) {
            if (t > amax) amax = t;
            if (strstr(nb, "tdie")) { tn++; tsum += t; if (t > tmax) tmax = t; }
            if (!summary) printf("%s=%.1f\n", nb, t);
        }
        CFRelease(e); if (name) CFRelease(name);
    }
    if (summary) printf("%.1f %.1f %d %.1f\n", tmax, tn ? tsum / tn : -1, tn, amax);
    return 0;
}
