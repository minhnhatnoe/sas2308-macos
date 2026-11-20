# Builds SAS2308HBA.kext with the Command Line Tools only (no Xcode).
#
#   make            build build/SAS2308HBA.kext (arm64e, ad-hoc signed)
#   make check      static checks: plist lint, signature, undefined symbols
#                   against the OSBundleLibraries (kextlibs/kmutil; loads nothing)
#   make analyze    clang static analyzer on the kext units and the core
#   make clean
#
# Nothing here loads the kext. See docs/LOADING.md for loading.

BUNDLE_ID := org.sas2308.driver.SAS2308HBA
VERSION   := 0.1.0
NAME      := SAS2308HBA

SDK   := $(shell xcrun --sdk macosx --show-sdk-path)
KFW   := $(SDK)/System/Library/Frameworks/Kernel.framework
# Kexts on Apple silicon are arm64e (pointer authentication).
ARCH  := arm64e
MINOS := 27.0

BUILD := build
KEXT  := $(BUILD)/$(NAME).kext
BIN   := $(KEXT)/Contents/MacOS/$(NAME)

COMMON := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
          -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector \
          -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
          -I$(KFW)/PrivateHeaders -I$(KFW)/Headers -I../core \
          -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          -Wno-inconsistent-missing-override
CXXFLAGS := $(COMMON) -std=gnu++20 -fapple-kext -fno-exceptions -fno-rtti -fno-threadsafe-statics \
            -DSAS2308_BUNDLE_ID='"$(BUNDLE_ID)"'
# The core, its compat layer and mps_xnu_os.cpp are linked into the kext but
# not exported: FreeBSD names such as msleep, wakeup, pause, DELAY or
# getmicrotime are also xnu BSD KPI names. Only the IOKit class (its
# vtable/metaclass symbols) and kmod_info stay global (nm check in `make check').
HIDDEN := -fvisibility=hidden
CFLAGS   := $(COMMON) -std=gnu11 -DKEXT_BUNDLE_ID=$(BUNDLE_ID) -DKEXT_VERSION='"$(VERSION)"'
LDFLAGS  := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
            -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
            -L$(SDK)/usr/lib -lkmodc++ -lkmod

# The FreeBSD-shaped core (plain C, kext flags) and its compat layer.
CORE_SRCS := ../core/compat/mps_compat.c ../core/compat/cam_compat.c ../core/mps.c ../core/mps_config.c \
             ../core/mps_sas.c ../core/mps_sas_lsi.c ../core/mps_table.c ../core/mps_xnu.c
CORE_HDRS := ../core/compat/mps_compat.h ../core/compat/cam_compat.h ../core/compat/queue.h \
             ../core/mpsvar.h ../core/mps_sas.h ../core/mps_table.h ../core/mps_xnu.h \
             ../core/mps_xnu_stats.h ../core/mps_mpi.h
CORE_OBJS := $(patsubst ../core/%.c,$(BUILD)/core/%.o,$(CORE_SRCS))
# -mkernel turns on conversion warnings FreeBSD's kernel build does not use
# (-Wsign-conversion, -Wshorten-64-to-32, -Wimplicit-int-conversion,
# -Wconditional-uninitialized); the ported code relies on implicit
# conversions like its upstream, so they are off for the core only.
CORE_CFLAGS := $(COMMON) $(HIDDEN) -std=gnu11 -Wno-sign-compare -Wno-sign-conversion -Wno-shorten-64-to-32 \
               -Wno-implicit-int-conversion -Wno-conditional-uninitialized

KEXT_SRCS := SAS2308HBA.cpp SAS2308HBA_PM.cpp SAS2308HBA_Hotplug.cpp SAS2308HBA_Props.cpp
KEXT_HDRS := SAS2308HBA.h SAS2308HBA_Targets.h mps_xnu_os.h ../core/mps_xnu_stats.h
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(KEXT_SRCS)) $(BUILD)/mps_xnu_os.o $(CORE_OBJS) $(BUILD)/SAS2308HBA_info.o

all: $(KEXT)

# The SAS2308HBA*.cpp units (IOKit, <sys/queue.h>) never include the core
# headers; mps_xnu_os.cpp is the only C++ unit that does (core/compat/queue.h).
$(BUILD)/%.o: %.cpp $(KEXT_HDRS) | $(BUILD)
	xcrun clang++ $(CXXFLAGS) -fvisibility-inlines-hidden -c $< -o $@
$(BUILD)/mps_xnu_os.o: mps_xnu_os.cpp mps_xnu_os.h $(CORE_HDRS) | $(BUILD)
	xcrun clang++ $(CXXFLAGS) $(HIDDEN) -c $< -o $@
$(BUILD)/core/%.o: ../core/%.c $(CORE_HDRS) | $(BUILD)
	@mkdir -p $(dir $@)
	xcrun clang $(CORE_CFLAGS) -c $< -o $@
$(BUILD)/SAS2308HBA_info.o: SAS2308HBA_info.c | $(BUILD)
	xcrun clang $(CFLAGS) -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	mkdir -p $(KEXT)/Contents/MacOS
	xcrun clang++ $(LDFLAGS) -o $(BIN) $(OBJS)
	cp Info.plist $(KEXT)/Contents/Info.plist
	codesign --force --sign - $(KEXT)
	@echo "built $(KEXT)"

$(BUILD):
	mkdir -p $(BUILD)

check: $(KEXT)
	plutil -lint $(KEXT)/Contents/Info.plist
	lipo -archs $(BIN)
	codesign --verify --verbose=2 $(KEXT)
	kextlibs -undef-symbols -xml $(KEXT) || true
	-kmutil libraries -p $(KEXT) --undef-symbols > $(BUILD)/km.log 2>&1; \
	  grep -c " in .*: com\.apple\." $(BUILD)/km.log; \
	  ! grep -E "kpi\.(private|unsupported)|not found" $(BUILD)/km.log
	@# exported (global, defined) symbols: the class and kmod_info only
	nm -gU $(BIN) | awk '{print $$3}' | sort > $(BUILD)/exports.txt
	@wc -l < $(BUILD)/exports.txt
	! grep -vE '^__ZNK?10SAS2308HBA|^__ZTVN?10SAS2308HBA|^_kmod_info$$' $(BUILD)/exports.txt
	@# kernel os_log drops any format with %{...} annotations (EINVAL in log_encode_fmt)
	! grep -n '%{' $(KEXT_SRCS) mps_xnu_os.cpp $(CORE_SRCS) ../core/*.h

# Default checkers plus the array-bound and cstring ones used since M3.
ANALYZE := --analyze -Xanalyzer -analyzer-output=text \
           -Xanalyzer -analyzer-checker=security.ArrayBound,alpha.unix.cstring
analyze: | $(BUILD)
	@for f in $(KEXT_SRCS) mps_xnu_os.cpp; do echo "  ANALYZE $$f"; \
	  xcrun clang++ $(CXXFLAGS) $(ANALYZE) -o /dev/null $$f || exit 1; done
	@for f in $(CORE_SRCS); do echo "  ANALYZE $$f"; \
	  xcrun clang $(CORE_CFLAGS) $(ANALYZE) -o /dev/null $$f || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all check analyze clean
