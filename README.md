# SAS2308HBA

A macOS kernel extension for the LSI/Avago SAS2308 HBA (LSI 9207-8i and
relatives), built on FreeBSD's `mps(4)` driver. The FreeBSD driver core runs
nearly unchanged on a small compat layer; the kext connects it to
IOSCSIParallelFamily.

**Status: experimental.** A classic kext for macOS 27 on Apple silicon, tested
on one setup only. It runs a 9207-8i in a Thunderbolt PCIe enclosure with SATA
disks (hot plug, sleep/wake; current results in
`docs/benchmarks-20261121-final.md`, the first run in `docs/benchmarks-20261120.md`).
Loading it needs Reduced Security and SIP disabled (`docs/LOADING.md`). Read
"Safety" below first; the hardware tests still to run are listed in
`docs/reviews/2026-11-freebsd-deviations.md`.

## Safety

- Loading it needs SIP disabled and Reduced Security with user-managed kernel
  extensions (Permissive Security). That turns off protections for the whole
  Mac: system files and kernel extensions can then be changed by anything
  running as root. Turn them back on (`docs/LOADING.md`) when you stop using it.
- Tested only on one setup: Mac mini (Mac18,5, macOS 27.0), LSI SAS 9207-8i
  (SAS2308, IT firmware 20.00.07.00) in a Thunderbolt 3 PCIe dock (JHL7440),
  7-8 Seagate Exos X18 ST18000NM000J SATA drives. Other cards, enclosures,
  expanders, SAS drives and macOS versions are untested.
- This is a kernel driver: a bug can panic the Mac, corrupt data or lose it.
  Keep backups and do not use it for data you cannot lose.
- No warranty: BSD-2-Clause, provided "as is" (`LICENSE`).
- **The benchmark scripts destroy data.** `bench/` scripts write to the raw
  disks (`/dev/rdiskN`) of every matching drive behind the HBA and overwrite
  everything on them. They refuse to run unless `SAS2308_YES_DESTROY` names
  exactly the selected disks; read `bench/README.md` before running any.

## Layout

| Path | What |
|---|---|
| `core/` | FreeBSD `sys/dev/mps` ported to xnu: every change is marked `/* XNU: ... */` |
| `core/compat/` | the FreeBSD kernel and CAM API subset the core uses |
| `core/mpi/` | LSI/Avago MPI2 headers, unmodified |
| `kext/` | `SAS2308HBA.kext`: IOKit glue, power management, hot plug, properties |
| `test/` | host tests: the core and the kext glue against an IOC model (Linux or macOS) |
| `tools/` | `sas2308ctl` (log level, counters), `sasbench`, FreeBSD import and diff scripts |
| `bench/` | benchmark scripts and raw results (`bench/README.md`; the scripts overwrite disks) |
| `docs/` | design notes, loading steps, benchmarks, review records (`docs/reviews/`) |

## Build and test

- Host tests (any Unix with a C/C++ compiler): `make -C test test`.
- Kext (on the Mac, Command Line Tools with the macOS SDK): `make -C kext`,
  `make -C kext check`; tools: `make -C tools`. Loading: `docs/LOADING.md`.
- Install / roll back / uninstall on the Mac: `mac/install.sh -n` and `mac/uninstall.sh -n` (dry runs first).

## FreeBSD upstream

`core/` is a vendor import of FreeBSD `sys/dev/mps` (release/14.5.0). The
branch `vendor/freebsd-mps` holds the pristine files; `git diff
vendor/freebsd-mps master -- core/` is the whole port. `core/VENDOR.txt` has
the pin, the path map and the update procedure (`tools/import-freebsd.sh`).

## Support

Provided as is. Issues are welcome and answered on a best-effort basis; there
is no promise of fixes or of support for other hardware. Reports from other
setups (card, firmware, enclosure, drives, macOS version; what worked, what did
not, with `sas2308ctl stats` and the driver's log lines) are especially welcome.

## License

BSD-2-Clause, see `LICENSE`, which also lists the third-party notices
(FreeBSD mps, LSI/Avago MPI2 headers, `queue.h`, CAM decoder tables).
