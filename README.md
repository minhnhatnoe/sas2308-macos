# SAS2308HBA

A macOS kernel extension for the LSI/Avago SAS2308 HBA (LSI 9207-8i and
relatives), built on FreeBSD's `mps(4)` driver. The FreeBSD driver core runs
nearly unchanged on a small compat layer; the kext connects it to
IOSCSIParallelFamily.

Status: a classic kext for macOS 27 on Apple silicon. It runs a 9207-8i in a
Thunderbolt PCIe enclosure with SATA disks (hot plug, sleep/wake,
benchmarks in `docs/benchmarks-20261120.md`). Loading it needs Reduced
Security and SIP disabled (`docs/LOADING.md`). Use at your own risk.

## Layout

| Path | What |
|---|---|
| `core/` | FreeBSD `sys/dev/mps` ported to xnu: every change is marked `/* XNU: ... */` |
| `core/compat/` | the FreeBSD kernel and CAM API subset the core uses |
| `core/mpi/` | LSI/Avago MPI2 headers, unmodified |
| `kext/` | `SAS2308HBA.kext`: IOKit glue, power management, hot plug, properties |
| `test/` | host tests: the core and the kext glue against an IOC model (Linux or macOS) |
| `tools/` | `sas2308ctl` (log level, counters), `sasbench`, FreeBSD import and diff scripts |
| `bench/` | benchmark scripts and raw results |
| `docs/` | design notes, loading steps, benchmarks, review records (`docs/reviews/`) |

## Build and test

- Host tests (any Unix with a C/C++ compiler): `make -C test test`.
- Kext (on the Mac, Command Line Tools with the macOS SDK): `make -C kext`,
  `make -C kext check`; tools: `make -C tools`. Loading: `docs/LOADING.md`.

## FreeBSD upstream

`core/` is a vendor import of FreeBSD `sys/dev/mps` (release/14.5.0). The
branch `vendor/freebsd-mps` holds the pristine files; `git diff
vendor/freebsd-mps master -- core/` is the whole port. `core/VENDOR.txt` has
the pin, the path map and the update procedure (`tools/import-freebsd.sh`).

## License

BSD-2-Clause, see `LICENSE`, which also lists the third-party notices
(FreeBSD mps, LSI/Avago MPI2 headers, `queue.h`, CAM decoder tables).
