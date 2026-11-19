/*
 * kmod_info for the kext. Xcode generates this file; without Xcode we
 * write it by hand. _start/_stop come from libkmodc++.a / libkmod.a.
 * KEXT_BUNDLE_ID and KEXT_VERSION are passed by the Makefile and must
 * match CFBundleIdentifier / CFBundleVersion in Info.plist.
 */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(KEXT_BUNDLE_ID, KEXT_VERSION, _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
