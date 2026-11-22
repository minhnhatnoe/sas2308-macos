# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# Top-level entry points (GNU make). Every target hands off to a
# subdirectory Makefile or a script under tools/; `make help` lists them.

OS := $(shell uname -s)
FREEBSD_REF := ref/freebsd-src

ifeq ($(OS),Darwin)
all: kext tools
else
all: test
endif

help:
	@echo 'make                  macOS: build the kext and tools; elsewhere: run the host tests'
	@echo 'make test             host tests (make -C test test; Linux or macOS)'
	@echo 'make kext             build kext/build/SAS2308HBA.kext and run its static checks (macOS)'
	@echo 'make tools            build tools/sas2308ctl (macOS)'
	@echo 'make freebsd-ref      clone or update $(FREEBSD_REF) at the release core/VENDOR.txt pins'
	@echo 'make install-dry-run  tools/install.sh --dry-run: print every install step, change nothing (macOS)'
	@echo 'make install          tools/install.sh: build, check and install the kext (macOS, sudo)'
	@echo 'make uninstall        tools/uninstall.sh (macOS, sudo)'
	@echo 'make clean            remove build products'

test:
	$(MAKE) -C test test

freebsd-ref:
	@want=$$(sed -n 's/^Release: *\(release\/[^ ]*\).*/\1/p' core/VENDOR.txt); \
	have=$$(git -C $(FREEBSD_REF) rev-parse -q --verify HEAD 2>/dev/null); \
	tagged=$$(git -C $(FREEBSD_REF) rev-parse -q --verify "refs/tags/$$want^{commit}" 2>/dev/null); \
	if [ -n "$$want" ] && [ -n "$$have" ] && [ "$$have" = "$$tagged" ]; then \
		echo "$(FREEBSD_REF) is at $$want"; \
	else \
		tools/fetch-freebsd-ref.sh; \
	fi

ifeq ($(OS),Darwin)
kext:
	$(MAKE) -C kext
	$(MAKE) -C kext check

tools:
	$(MAKE) -C tools

install:
	tools/install.sh

install-dry-run:
	tools/install.sh --dry-run

uninstall:
	tools/uninstall.sh

clean:
	$(MAKE) -C test clean
	$(MAKE) -C kext clean
	$(MAKE) -C tools clean
else
kext tools install install-dry-run uninstall:
	@echo "make $@: macOS only (this is $(OS))" >&2; exit 1

clean:
	$(MAKE) -C test clean
	$(MAKE) -C tools clean
endif

.PHONY: all help test freebsd-ref kext tools install install-dry-run uninstall clean
