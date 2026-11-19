# sas2308ctl: runtime log level / counters for SAS2308HBA (macOS userland).
sas2308ctl: sas2308ctl.c
	xcrun clang -O2 -Wall -Wextra -o $@ $< -framework IOKit -framework CoreFoundation

clean:
	rm -f sas2308ctl

.PHONY: clean
