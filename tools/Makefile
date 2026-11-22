# sas2308ctl: runtime log level / counters for SAS2308HBA (macOS userland).
# sasbench: raw-disk benchmark used by the bench/ scripts (`make -C tools sasbench`).
sas2308ctl: sas2308ctl.c
	xcrun clang -O2 -Wall -Wextra -o $@ $< -framework IOKit -framework CoreFoundation -framework DiskArbitration

sasbench: sasbench.c
	xcrun clang -O2 -pthread -o $@ $<

clean:
	rm -f sas2308ctl sasbench

.PHONY: clean
