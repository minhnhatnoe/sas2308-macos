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
| `tools/` | `sas2308ctl` (log level, counters), `sasbench`, install and uninstall scripts, FreeBSD fetch, import and diff scripts |
| `bench/` | benchmark scripts and raw results (`bench/README.md`; the scripts overwrite disks) |
| `docs/` | design notes, loading steps, benchmarks, review records (`docs/reviews/`) |
| `ref/` | not in the repository: `make freebsd-ref` creates `ref/freebsd-src` (ignored by git) |

## Build and test

From the repository root (GNU make; `make help` lists the targets):

- `make`: on the Mac (Command Line Tools with the macOS SDK) builds
  `kext/build/SAS2308HBA.kext`, runs its static checks (`make -C kext check`)
  and builds `tools/sas2308ctl`; elsewhere it runs the host tests.
- `make test`: host tests (any Unix with a C/C++ compiler; `make -C test test`).
- `make install-dry-run`, then `make install`; `make uninstall` (on the Mac,
  `tools/install.sh` and `tools/uninstall.sh`). Loading needs the security
  change in `docs/LOADING.md`, which also covers rollback and `--verify`.
- `make freebsd-ref`: the FreeBSD source `core/VENDOR.txt` pins, as a shallow,
  sparse clone of `sys/dev/mps` and `sys/cam` in `ref/freebsd-src` (about
  24 MB; more paths with `tools/fetch-freebsd-ref.sh sys/sys`). It is created
  on demand and is not part of the repository; the build and tests do not
  need it.

## FreeBSD upstream

`core/` is a vendor import of FreeBSD `sys/dev/mps` (release/15.1.0). The
branch `vendor/freebsd-mps` holds the pristine files; `git diff
vendor/freebsd-mps master -- core/` is the whole port. `core/VENDOR.txt` has
the pin, the path map and the update procedure (`tools/import-freebsd.sh`).
`make freebsd-ref` fetches the pinned upstream tree into `ref/freebsd-src`
for reading or for `REF=` in `tools/diff-vs-freebsd.sh`; the import script
creates it itself when it is missing.

## Support

Provided as is. Issues are welcome and answered on a best-effort basis; there
is no promise of fixes or of support for other hardware. Reports from other
setups (card, firmware, enclosure, drives, macOS version; what worked, what did
not, with `sas2308ctl stats` and the driver's log lines) are especially welcome.

## License

BSD-2-Clause, see `LICENSE`. Third-party code (FreeBSD mps, LSI/Avago MPI2
headers, `queue.h`, CAM decoder tables) keeps its own notice in its file
headers; `core/VENDOR.txt` describes the FreeBSD import.
