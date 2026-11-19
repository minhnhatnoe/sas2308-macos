# Builds SAS2308HBA.kext with the Command Line Tools only (no Xcode).
#
#   make            build build/SAS2308HBA.kext (arm64e, ad-hoc signed)
#   make check      static checks: plist lint, signature, undefined symbols
#                   against the OSBundleLibraries (kextlibs/kmutil; loads nothing)
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
CXXFLAGS := $(COMMON) -std=gnu++20 -fapple-kext -fno-exceptions -fno-rtti -fno-threadsafe-statics
CFLAGS   := $(COMMON) -std=gnu11 -DKEXT_BUNDLE_ID=$(BUNDLE_ID) -DKEXT_VERSION='"$(VERSION)"'
LDFLAGS  := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
            -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
            -L$(SDK)/usr/lib -lkmodc++ -lkmod

# The FreeBSD-shaped core (plain C, kext flags) and its compat layer.
CORE_SRCS := ../core/compat/mps_compat.c ../core/compat/cam_compat.c ../core/mps.c \
             ../core/mps_sas.c ../core/mps_sas_lsi.c ../core/mps_table.c ../core/mps_xnu.c
CORE_HDRS := ../core/compat/mps_compat.h ../core/compat/cam_compat.h ../core/compat/queue.h \
             ../core/mpsvar.h ../core/mps_sas.h ../core/mps_table.h ../core/mps_xnu.h \
             ../core/mps_xnu_stats.h ../core/mps_mpi.h
CORE_OBJS := $(patsubst ../core/%.c,$(BUILD)/core/%.o,$(CORE_SRCS))
# -mkernel turns on conversion warnings FreeBSD's kernel build does not use
# (-Wsign-conversion, -Wshorten-64-to-32, -Wimplicit-int-conversion,
# -Wconditional-uninitialized); the ported code relies on implicit
# conversions like its upstream, so they are off for the core only.
CORE_CFLAGS := $(COMMON) -std=gnu11 -Wno-sign-compare -Wno-sign-conversion -Wno-shorten-64-to-32 \
               -Wno-implicit-int-conversion -Wno-conditional-uninitialized

OBJS := $(BUILD)/SAS2308HBA.o $(BUILD)/mps_xnu_os.o $(CORE_OBJS) $(BUILD)/SAS2308HBA_info.o

all: $(KEXT)

# SAS2308HBA.cpp (IOKit, <sys/queue.h>) never includes the core headers;
# mps_xnu_os.cpp is the only C++ unit that does (core/compat/queue.h).
$(BUILD)/SAS2308HBA.o: SAS2308HBA.cpp SAS2308HBA.h mps_xnu_os.h ../core/mps_xnu_stats.h | $(BUILD)
	xcrun clang++ $(CXXFLAGS) -c $< -o $@
$(BUILD)/mps_xnu_os.o: mps_xnu_os.cpp mps_xnu_os.h $(CORE_HDRS) | $(BUILD)
	xcrun clang++ $(CXXFLAGS) -c $< -o $@
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

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
