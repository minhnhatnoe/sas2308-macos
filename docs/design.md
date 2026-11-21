# SAS2308HBA.kext — design notes

Development stages, as named in this file and in older comments (the code of M1-M4, a driver
written from scratch before the port of FreeBSD's mps, is only in the tag `archive/pre-rewrite`;
master starts from the FreeBSD vendor import):

| Stage | What |
|---|---|
| M1 | first driver: IOC init, queues, SCSI IO path, mock IOC test |
| M2 | task management, timeout escalation, IOC reinit, multi-LUN |
| M3 | MPI2 chain SGLs, derived segment limits |
| M4 | asynchronous recovery, FreeBSD-style logging, statistics |
| M5a | the FreeBSD-shaped core: compat layer, port of sys/dev/mps, the kext on it |
| M5b | Mac conventions: kext layout, power management, shutdown, termination, properties |
| M5c | hot-plug page 0 failures, recovered-sense noise, IOC temperature |

Review findings are written "review C1", "review M3" (ids of `docs/reviews/2026-11-final.md`);
an M-id in a review context is a finding, not a stage. The test groups `M1`-`M4` of
`mock_ioc_test` are named after the stages.

Target: LSI SAS 9207-8i (Broadcom SAS2308, PCI 1000:0087, MPI 2.0 IT firmware) in a
Thunderbolt 3 PCIe dock (JHL7440, PCIe 3.0 x4 tunnel) on an Apple-silicon Mac mini
(Mac18,5, M6, macOS 27.0). Classic kext, because the Mac has no Xcode/Apple ID (so no
DriverKit SDK / iig). Built with Command Line Tools only (`kext/Makefile`, arm64e, `-mkernel`).

## Layout
- `core/` — the FreeBSD-shaped core (M5): `mps.c`, `mps_sas.c`, `mps_sas_lsi.c`, `mps_table.c`
  (ported from FreeBSD `sys/dev/mps`), `mpsvar.h`, `mps_sas.h`, `mps_table.h`, `mps_mpi.h`;
  `mps_xnu.{c,h}` + `mps_xnu_stats.h` (what FreeBSD has no counterpart for); `compat/`
  (`mps_compat.{c,h}`: FreeBSD kernel API subset, `cam_compat.{c,h}`: thin CAM, `queue.h`).
  The M1–M4 C++ core (`MpsCore`, `MpsDecode`) was removed in M5a-2; the M1–M4 sections below
  describe the behaviour the port keeps (old → new names in "What is implemented").
- `core/mpi/` — MPI2 headers from FreeBSD (`core/VENDOR.txt` has the commit).
- `kext/SAS2308HBA*.{h,cpp}` — the `IOSCSIParallelInterfaceController` subclass, one file per
  concern since M5b (IntelMausi / itlwm layout): `SAS2308HBA.cpp` life cycle and family callbacks
  (capabilities, interrupts, IO, TMs), `SAS2308HBA_PM.cpp` power management and shutdown,
  `SAS2308HBA_Hotplug.cpp` targets and their properties, `SAS2308HBA_Props.cpp` registry
  properties (logging control, controller characteristics, statistics), `SAS2308HBA_Targets.h`
  the target table (no IOKit, host-tested). None includes a core header (IOService.h pulls in
  `<sys/queue.h>`).
- `kext/mps_xnu_os.{h,cpp}` — the only kext unit that includes the core: the `mps_os_*` hooks
  on xnu and the `mps_kext_*` calls (plain types) the class uses (FreeBSD `mps_pci.c`'s role).
- `tools/sas2308ctl.c` — userland: change the log level at runtime, print counters (M4).
- `tools/diff-vs-freebsd.sh` — `diff -u` of the ported files against FreeBSD, with a summary.
- `test/mock_ioc_test.cpp`, `test/mps_port_test.cpp` — the core against the software IOC
  model (`test/mock_ioc.h`) under ASan/UBSan/LSan; host hooks in `test/mps_host_os.*`;
  `test/kext_targets_test.cpp` — the kext's target table; `test/kext_pcie_test.cpp` — the "PCIe Link"
  decoding; `test/plist_test.sh` — the personality's device ids;
  `make -C test freestanding` compiles the core like the kext (`-nostdinc`, `-Werror`).

## M5: FreeBSD-shaped core
Goal: the core reads like FreeBSD `sys/dev/mps` so it can be reviewed with `diff -u`
(`tools/diff-vs-freebsd.sh`), the way OpenIntelWireless keeps OpenBSD iwm/iwx nearly
verbatim behind a compat layer. Plain C, style(9), FreeBSD identifiers, FreeBSD control
flow and comments. Unused FreeBSD functions are deleted, not `#if 0`'d. Every
intentional difference carries a short `/* XNU: ... */` comment (or `XNU:` in a block
comment) so it shows up in the diff.
Done in three steps: M5a-1 (compat layer, IOC side, discovery), M5a-2 (IO path, recovery, mock
test on the new core, C++ core deleted), M5a-3 (the kext on the new core, Mac verification).

### Layout
| File | Origin | Notes |
|---|---|---|
| `core/mps_config.c` | `mps_config.c` | `mps_config_get_sas_device_pg0` only (M5b) |
| `core/mps.c` | `mps.c` | IOC reset/ready, IOCFacts sizing, queues, commands, chains, SGEs (`mps_push_sge`, `mps_add_chain`, `mps_data_cb`, `mps_map_command`), `mps_intr_locked`, events, `mps_reinit`, `mps_periodic`, `mps_attach`/`mps_free`, `mps_parse_debug` |
| `core/mps_sas.c` | `mps_sas.c` | `mps_attach_sas`, `mpssas_action`, `mpssas_action_scsiio`, `mpssas_scsiio_complete`, `mpssas_scsiio_timeout`, TMs (`mpssas_send_abort`, `mpssas_send_reset`, `*_complete`, `mpssas_tm_timeout`, `mpssas_prepare_for_tm`), `mpssas_handle_reinit`, port enable, startup refcount |
| `core/mps_sas_lsi.c` | `mps_sas_lsi.c` | `mpssas_evt_handler`, `mpssas_fw_work` (topology, device status), `mpssas_add_device` |
| `core/mps_table.c`, `mps_table.h` | same | tables, `mps_describe_table`, `mps_print_*` |
| `core/mpsvar.h`, `core/mps_sas.h` | same | `struct mps_softc`, `struct mps_command`, `struct mpssas_softc/target`, inlines |
| `core/mps_xnu.c`, `mps_xnu.h`, `mps_xnu_stats.h` | — (new) | things FreeBSD has no counterpart for: log-level presets, failed-IO rate limit, CDB/sense/loginfo decoding (FreeBSD uses CAM's), statistics, simple target-id mapping (FreeBSD `mps_mapping.c` needs config pages), OS task-management entry |
| `core/compat/mps_compat.{h,c}`, `queue.h`, `cam_compat.{h,c}` | — (new) | FreeBSD kernel + CAM API subset, implemented once on top of the `mps_os_*` hooks |
| `kext/mps_xnu_os.{h,cpp}` | `mps_pci.c` (role) | `mps_os_*` on xnu/IOKit; `mps_kext_*`: attach/detach in the `Rig` order, ccbs for tasks and TMs, done queue, async → hotplug, statistics snapshot |
| `kext/SAS2308HBA.{h,cpp}` | — | the IOKit class (CAM periph's role) |
| `test/mps_host_os.cpp` | — | `mps_os_*` on the host (virtual clock, mock IOC) |

Ported files include `compat/mps_compat.h` (and `compat/cam_compat.h`) instead of FreeBSD's
`<sys/...>`/`<cam/...>` includes, and `"mpi/mpi2_*.h"` instead of `<dev/mps/mpi/...>`.

### Compat layer contract (final, M5a-3)
FreeBSD API → implementation (shared code in `core/compat/mps_compat.c` / `cam_compat.c`,
platform hooks `mps_os_*` with C linkage: `kext/mps_xnu_os.cpp` on xnu, `test/mps_host_os.cpp`
on the host). xnu column: public KPIs only (`kmutil` check below).

| FreeBSD | Shared compat | xnu (`kext/mps_xnu_os.cpp`) | host (`test/`) |
|---|---|---|---|
| `struct mtx`, `mtx_init/lock/unlock/owned/assert/destroy` | owner tracking (`mtx_owner` = `mps_os_curthread()`), mutex registry for `DELAY` | `IOLockAlloc/Lock/Unlock/Free` (lck_mtx), owner = `current_thread()` | flag, single thread |
| `msleep(chan, mtx, pri, wmesg, timo)`, `wakeup`, `pause` | ticks → µs, `hz` = 1000 | `IOLockSleep` / `IOLockSleepDeadline` (`THREAD_UNINT`, `clock_interval_to_deadline`), `THREAD_TIMED_OUT` → `EWOULDBLOCK`; `IOLockWakeup` (its lock argument is unused by xnu; one lock per kext load is passed); `pause` → `IOSleep` | advance virtual clock, run the "other threads" hook |
| `DELAY(us)` | the caller's locks stay held, as FreeBSD's spin (D16; until batch C a held `mtx` was dropped for ≥ 1 ms) | `IOSleep(ms)` + `IODelay(rest)` | virtual clock + hook |
| `ticks`, `sbinuptime`, `getmicrouptime`, `getmicrotime` | from `mps_os_uptime_us`, `mps_os_walltime` | `mach_absolute_time` → `absolutetime_to_nanoseconds`; `clock_get_calendar_microtime` | virtual |
| `malloc(9)`/`free(9)` (`M_ZERO`, `M_NOWAIT`, `M_WAITOK`) | 32-byte header (size, magic, the map the block is loaded on: `free` drops that DMA mapping, D6), zeroing, live count | `IOMalloc`/`IOFree` (mps_mtx is a blocking mutex, so allocation may block) | `malloc`/`free` |
| `bus_space_read_4/write_4` (`mps_regread/regwrite`) | — | volatile 32-bit MMIO at `mps_bhandle` = BAR1 virtual address (`mapDeviceMemoryWithRegister`); no mapping reads all-ones | mock registers |
| `bus_dma_template_*`, `bus_dma_tag_create/destroy`, `bus_dmamem_alloc/free`, `bus_dmamap_load/unload/create/destroy/sync`, `bus_dmamap_load_ccb` | tags carry align/lowaddr/boundary (inherited, the smaller one: D41)/maxsize/nsegments/maxsegsz; `load` takes `bus_dmamem_alloc` memory or a core `malloc` block (config page, IDENTIFY: `mps_os_dmamap_load_buf` maps it; released at unload, next load, `free`, destroy: D6); errors as busdma (callback, return 0 except ENOMEM: D42); `load_ccb` checks and passes a `CAM_DATA_SG_PADDR` list through, split at the tag's boundary (D41); `sync` is a no-op (DART PCIe DMA is coherent) | `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` (4 KiB aligned; physically contiguous and below 4 GiB only without a mapper) + `IODMACommand::withSpecification(kMapped, 32 or 64 address bits, maxsegsz, mapper)` on `IOMapper::copyMapperForDevice` (DART): IOVA < 4 GiB when `lowaddr` is 32-bit, as many segments as the tag allows (chain pool), every segment checked against alignment/lowaddr/maxsegsz; the list is computed at alloc, `load` copies it | mock regions, 4 KiB pieces |
| `callout_init_mtx`, `callout_reset[_sbt]`, `callout_stop/drain/pending` | sorted list per `mtx`, run with the mtx held by `mps_compat_callout_run(mtx)` | one `thread_call` per mtx, `thread_call_enter_delayed` at the earliest deadline (`nanoseconds_to_absolutetime`, +1 µs so it never fires early), `thread_call_cancel` to disarm; queued/running invocations counted so `mtx_destroy` waits for them (`thread_call_cancel_wait` is `kpi.private`) | the test advances time and calls `mps_compat_callout_run` |
| `taskqueue_create/enqueue/drain/free`, `TASK_INIT` | FIFO, tasks run without the mtx; `mps_compat_taskqueue_thread` = FreeBSD's `taskqueue_thread_loop` (sleeps on the queue, runs it, leaves after `taskqueue_free` once the queue is empty); drain sleeps until the task is neither queued nor running (D39) | one kernel thread per queue (`kernel_thread_start`), a kick is a wakeup; `taskqueue_free` waits for the thread to leave (`tq_exited`) | no threads: the test runs a queue when kicked (`tq_active` keeps nested invocations out) |
| `config_intrhook_establish` | list; `mps_compat_intrhook_run(arg)` runs the hooks of one instance (`ich_arg`, D40) | called from `StartController` after `EnableInterrupt()` (mps_startup: unmask, port enable) | `Rig::Start` |
| `TUNABLE_INT_FETCH`, `TUNABLE_STR_FETCH` (kenv) | `hw.mps.X` / `dev.mps.N.X` read as `mps.X` / `mps.N.X` through `mps_os_getenv` (D33) | boot-arg of that name (`PE_parse_boot_arg_str`), else the personality's property (Info.plist: `mps.max_io_pages` 256) | `gHost.tunables` (defaults = Info.plist's) |
| `device_printf`, `mps_dprint`, `printf`, `hexdump`, `snprintf` | kernel printf formats incl. `%b` (`mps_compat_vsnprintf`, D27); one `mps_os_log(dev, class, line)` per line | `os_log_with_type`, one handle per class (category = most specific class), type ERROR/DEFAULT/DEBUG as M4; `IOLog` with `sas2308_iolog=1` | captured |
| `KASSERT`, `panic` | file/line + message | logged as a `fault` line, no panic (M2–M4 rule) | aborts the test |
| `mps_pci_setup_interrupts/free_interrupts`, `mps_pci_restore` (mps_pci.c) | `mps_xnu.c` → `mps_os_*` | the family's `IOFilterInterruptEventSource` (MSI index chosen in `CreateDeviceInterrupt`), enabled in `StartController`, `HandleInterruptRequest` → `mps_intr_msi`; restore = memory decode + bus mastering on; `mps_os_controller_dead` = bus mastering off | counters |
| CAM: `union ccb`, `xpt_action`, `xpt_done`, `xpt_freeze/release_devq`, `xpt_freeze/release_simq`, `CAM_REQUEUE_REQ`, `xpt_rescan`, `xpt_async`, `xpt_register_async` | thin CAM (CAM's func codes and values, D37): a ccb finished with `CAM_REQUEUE_REQ` or sent to a frozen sim/devq is held in arrival order and re-sent to `mpssas_action` on release (M4's *held IO*); a release by a completion runs the held queue once, when mps_mtx is let go, and a frozen SIM stops a run (D35); a requeue must be backed by a freeze (assert, D67); a freeze through the SIM's own path (`xpt_periph`, the TM ccb) holds the whole target (D17); `xpt_done` calls `ccb_h.cbfcnp` with mps_mtx held; rescan/async call the registered callbacks | `XPT_SCSI_IO` ccb in each task's HBA data (CDB, LUN number, `CAM_DATA_SG_PADDR` list from `gen64IOVMSegments` in segments after the ccb, as many as `buffer_dmat` allows: D32; ccb sense buffer); cbfcnp queues the task for `DeliverCompletions` (no family call under mps_mtx); `XPT_RESET_DEV` for OS LUN/target resets (D10); `AC_XNU_SCAN` → `mps_xnu_target_present` sampled → hotplug thread | test builds ccbs |

`hz` = 1000 (one tick = 1 ms). `PAGE_SIZE` inside the core is 4096 (MPI2 SGE sizing,
FreeBSD `maxio` arithmetic), independent of the 16 KiB arm64 page.

### Rules for the port
- Keep M1–M4 behaviour; where FreeBSD's own code does the job, use FreeBSD's code and
  adapt the mock test's expectations; where we deviate, `/* XNU: ... */`.
- Deviations that stay (from M2–M4): no `panic` (failed reinit → controller disabled,
  IO failed `CAM_DEV_NOT_THERE`); reinit runs on a thread (taskqueue), not in callout
  context, and retries the reset on a nested fault (≤ 3 attempts); waits drop `mps_mtx`
  (reinit sets `MPS_FLAGS_DIAGRESET` first; `mps_intr_locked` and new IO honour it);
  all-ones register reads = surprise removal (`MPS_FLAGS_XNU_GONE`) once confirmed (D11:
  doorbell and PCI vendor id re-read 5 times 20 ms apart), checked by every
  wait loop; `MPS_FLAGS_SHUTDOWN` abandons a sleeping reinit; reply-descriptor hardening
  (SMID range, reply address range/alignment, idle SMID ignored and counted); a target
  recovery that finds no free high-priority SMID waits for one instead of being dropped;
  OS-requested TMs go FreeBSD's ways (D10: target/LUN reset = `XPT_RESET_DEV`, abort = the
  task's timeout recovery, the rest rejected); IOs held during recovery (CAM requeue);
  SAS Device Page 0 / persistent mapping not read (simple target-id mapping).
- `mps_get_tunables` is FreeBSD's (batch C, D33): FreeBSD's defaults, then the loader
  tunables as `mps.X` / `mps.N.X` boot-args or personality keys; the personality sets
  `mps.max_io_pages` 256 (1 MiB per IO, D32; FreeBSD's default -1 would size every task
  for the IOC's 4.5 MiB); the queue sizes are FreeBSD's since branch queue-freebsd (see
  "Queue sizes as FreeBSD"; the kext keeps 8 normal frames from the OS: D21).

### Status and decisions (M5a-1)
Done: `core/compat/` (kernel API + thin CAM), `mps.c` (whole IOC side), `mps_table.c`,
`mpsvar.h`, `mps_sas.h`, attach/discovery/reinit/removal parts of `mps_sas.c` and
`mps_sas_lsi.c`, `mps_xnu.c`; host smoke test `test/mps_port_test` (ASan/UBSan/LSan).
`mpssas_action_scsiio`/`mpssas_action_resetdev` are placeholders (`CAM_REQ_INVALID`); the
IO path, TMs/recovery, the kext glue and the switch of `mock_ioc_test` come next.
`MpsCore`/`MpsDecode` still build the kext and `mock_ioc_test`.
- Commands are FreeBSD's free lists: SMIDs 1..4 high priority (TMs), 5..75 normal (IO and
  internal: events, acks, port enable, REMOVE_DEVICE). 72 + 4 request frames, 107 replies,
  queues 112 deep, chain pool 29 × 72 = 2088 frames, one `bus_dmamem_alloc` loaded as many
  4 KiB segments (`mps_load_chains_cb`).
- Chain SGLs use FreeBSD's `mps_add_chain`/`mps_push_sge` format (chain Length = frame size,
  NextChainOffset 0, request ChainOffset 0; 1 simple SGE + chain in the request frame,
  9 per chain frame). M3's Linux-style chain fields are dropped; adapt the mock's SGL walker.
- Reinit: `mps_periodic` (1 s callout) queues `sc->mps_reinit_task` on `sc->mps_tq`;
  `mps_reinit` retries diag reset + IOCFacts/IOCInit up to `MPS_XNU_REINIT_TRIES`, then
  `mps_xnu_disable` (GONE, `mps_os_controller_dead`, held ccbs failed `CAM_DEV_NOT_THERE`,
  wildcard rescan). `mps_wait_command` still calls `mps_reinit` inline on a timeout (thread
  context). Faults are found by `mps_periodic` only (M2–M4 also polled on interrupts).
- Thin CAM: only `XPT_SCSI_IO` is held while queues are frozen; `cbfcnp` and async callbacks
  run with `mps_mtx` held, so the kext must defer family calls. Rescans arrive as
  `AC_XNU_SCAN` (arg = the scan ccb); the OS then asks `mps_xnu_target_present(sc, tid)`.
  Data comes as `CAM_DATA_SG_PADDR` (`bus_dma_segment_t[]` in `data_ptr`, `sglist_cnt`);
  more segments than `buffer_dmat` allows → `bus_dmamap_load_ccb` returns `EFBIG`.
- C++ users (kext glue, tests) define `MPS_COMPAT_NO_LIBC_REMAP` and include the core
  headers inside `extern "C" {}`; `compat/queue.h` clashes with IOKit's `<sys/queue.h>`,
  so the glue keeps them in separate translation units.
- `make -C test freestanding` compiles the C core like the kext (`-nostdinc`, `-Werror`).

### Status and decisions (M5a-2)
Done: the IO path and recovery in `mps_sas.c` (`mpssas_action_scsiio`, `mpssas_scsiio_complete`,
`mpssas_scsiio_timeout`, `mps_sc_failed_io_info`, `mpssas_send_abort/_reset`, the three
`*_complete`, `mpssas_tm_timeout`, `mpssas_action_resetdev/_complete`); `mock_ioc_test` runs on
the new core (ccbs through `xpt_action`); `MpsCore`/`MpsDecode` deleted (`kext/` still uses
them and does not build until the next milestone — done in M5a-3). `core/mps_mpi.h` stays (the core includes it).
XNU deviations (all marked in the code):
- Every `mps_reinit()` call in the IO path is `mps_xnu_request_reinit()` (queues
  `sc->mps_reinit_task`, sets `xnu.reinit_pending`, cleared when `mps_reinit` starts). New SCSI
  IO is requeued (SIM queue frozen, `MPSSAS_QUEUE_FROZEN`) while a reinit is pending or running;
  OS TMs get `CAM_BUSY` then. Gone → `CAM_DEV_NOT_THERE`.
- `mpssas_prepare_for_tm` freezes the whole target (wildcard LUN), once per TM, released by
  `mpssas_free_tm`; an escalation reuses the TM's ccb (FreeBSD leaks it). Without this the thin
  CAM, which releases a requeued ccb's own freeze at once, would spin on `INRESET`.
- `mpssas_tm_timeout` marks the TM (`MPS_CM_FLAGS_XNU_TM_TIMEDOUT`): a reply before the reinit is
  counted as late (`tm_late_replies`) and ignored; the reinit's `mpssas_complete_all_commands`
  frees the TM. An OS TM's ccb completes at the timeout with `CAM_CMD_TIMEOUT`.
- Recoveries that found no free TM start from `mpssas_free_tm` (`mpssas_xnu_retry_recovery`).
- OS task management (replaced in batch B, D10: `XPT_RESET_DEV` / the timeout recovery): `XPT_XNU_TM` ccb (`struct ccb_xnu_tm`: task_type, abort_ccb; results
  ioc_status/response_code/termination_count), all six types, FreeBSD's TM timeouts (abort 5 s,
  resets 30 s); an abort of a ccb still held by the thin CAM completes it (`CAM_REQ_ABORTED`)
  without the IOC. OS timeout of a task: `mps_xnu_ccb_timeout(sc, ccb)` (held → `CAM_CMD_TIMEOUT`
  at once; posted → `mpssas_scsiio_timeout`; `ENOENT` if already done). `ccb_h.timeout` 0 = no
  core callout (the kext times tasks); non-zero arms FreeBSD's callout.
- `bus_dmamap_load_ccb` refuses (no callback) a list that does not add up to `dxfer_len`, a
  data ccb without direction (`EINVAL` → `CAM_REQ_INVALID`), more segments than the tag or a
  segment > 16 MiB − 1 (`EFBIG` → `CAM_REQ_TOO_BIG`); `mpssas_action_scsiio` undoes and fails.
- `buffer_dmat` segments capped at (frame/12 − 1) × MaxChainDepth + 1 (FreeBSD's `+ 1` can
  exceed the chain depth); request frames < 128 bytes refused; `num_chains` ≥ 1; unaligned
  TM LUN store and an `int` shift overflow fixed (UBSan).
- `mps_intr_locked` checks the doorbell for FAULT when it finds nothing (M2–M4),
  `mps_xnu_check_fault`. Gone (all-ones) → reinit task → `mps_xnu_disable` →
  `mpssas_xnu_fail_all` (all commands with NULL reply, SIM freezes dropped, held ccbs failed).
  `mps_free` stops the IOC (MUR) and calls `mpssas_xnu_fail_all` before detaching: every ccb
  completes once. NULL-reply completions treat gone/detaching like a reset (`MPS_XNU_RESETTING`).
- Statistics and lines with no FreeBSD counterpart in `mps_xnu.c`: `mps_xnu_io_start/_done`
  (trace lines, M4 counters, the rate-limited failed-IO line decoding the IOC's sense buffer),
  `mps_xnu_tm_done` (TM line, `tm_failed`), `mps_xnu_periodic` (summaries, `held` from the SIM).
  No EEDP, TLR, WarpDrive, SSU, SMP passthrough (deleted with `XNU:` notes).
- `mps_attach` calls `mps_free` itself on every failure (FreeBSD did it inside
  `mps_iocfacts_allocate` on two paths and again in `mps_pci_attach`); the caller never does.

Test expectations changed (FreeBSD behaves differently; everything else kept):
- Statuses: success `CAM_REQ_CMP`; terminated by our recovery `CAM_CMD_TIMEOUT`, by an OS TM
  `CAM_REQ_ABORTED`; IO failed by a reinit (also a timed-out one) `CAM_SCSI_BUS_RESET`; gone /
  detach `CAM_DEV_NOT_THERE`; IO_DATA_ERROR `CAM_REQ_CMP_ERR` without ccb sense (FreeBSD copies
  sense only for success/underrun/recovered). TM results: timeout `CAM_CMD_TIMEOUT`, in flight at
  a reinit `CAM_REQ_ABORTED`, no device `CAM_DEV_NOT_THERE`, during a reinit `CAM_BUSY`.
- SMIDs from the free lists: TM 1–4, normal 5–75 (71 shared by IO and internal commands).
  Checks on caller-chosen/reserved SMIDs became: each IO its own SMID, `CAM_TID_INVALID`,
  `CAM_LUN_INVALID`.
- SGL: 1 SGE in the request frame, 9 per chain frame → chains = ⌈(n − 1)/9⌉ (2 segments: 1
  chain); 257 segments (maxio 1 MiB + 1), pool 72 × 29 = 2088. IOCFacts table (depth 0, 1, 3;
  256-byte frames depth 2): 1, 10, 28, 41 segments (was 2, 11, 29, 53).
- Chain pool exhaustion (`max_chains` 5): requeue + frozen SIM queue instead of a refusal; later IO
  waits for a completion; `chain_alloc_fail` 1 (was 2); maxio not reduced (257 segments, was 47).
- TM late reply: the TM stays busy until the reinit; a duplicate of it counts as late, not stale.
- Nested fault: injected at IOCInit (FreeBSD registers events after the reinit loop, so a fault
  there is a second reinit); one fault counted. Fault on an empty queue needs an explicit
  interrupt in the test. Reinit failure: 3 diag resets + 1 at attach.
- Logging: FreeBSD wording for timeouts/aborts/resets/attach; `mps_parse_debug("bogus")` clears
  the mask (FreeBSD); trace-level checks look for the two XNU lines instead of counting lines.
- Detach with TMs: late replies after `mps_free` cannot be posted (softc freed) → checks that the
  interrupts are torn down. Upcalls (`IocReset`, `RediscoveryDone`, `DeviceArrived`,
  `RecoveryDone`) → `AC_BUS_RESET`, port enables + `mps_xnu_target_present`, `AC_XNU_SCAN`,
  `recovered_*` counters.
- Mock: FreeBSD SGL walker (chain Length = frame size, NextChainOffset/ChainOffset 0,
  LAST_ELEMENT only before a chain, depth ≤ MaxChainDepth, every address mapped, data moved);
  topology event length 3 + n dwords (was 5: FreeBSD copies `EventDataLength`); a faulted IOC
  ignores handshakes; `faultOnIocInitOnce`.

Totals: `mock_ioc_test` 38 scenarios, 405 CHECK sites (+ 39 in `mock_ioc.h`), every ccb
checked for exactly one completion at detach; `mps_port_test` 10 scenarios, 141 checks. Both
PASS under ASan/UBSan/LSan; `make freestanding` clean; `gcc -fanalyzer` on the core C clean
(5 NULL-SIM paths in the thin CAM fixed).

| file | freebsd | ours | common | added | removed | xnu |
|---|---|---|---|---|---|---|
| mpsvar.h | 836 | 800 | 759 | 41 | 77 | 19 |
| mps_sas.h | 170 | 168 | 166 | 2 | 4 | 1 |
| mps_table.h | 66 | 62 | 62 | 0 | 4 | 0 |
| mps.c | 3232 | 2862 | 2614 | 248 | 618 | 63 |
| mps_sas.c | 3366 | 2556 | 2123 | 433 | 1243 | 75 |
| mps_sas_lsi.c | 1286 | 360 | 338 | 22 | 948 | 6 |
| mps_table.c | 533 | 428 | 416 | 12 | 117 | 5 |
| total | 9489 | 7236 | 6478 | 758 | 3011 | 169 |

### Status and decisions (M5a-3): the kext on the FreeBSD core
M5 is complete: the kext builds on the core above; `MpsCore`/`MpsDecode` are gone from every
build. The restyle of the kext itself (PM, `willTerminate`, per-target properties, file split)
is a separate milestone; M5a-3 changed `kext/SAS2308HBA.{h,cpp}` only as far as the new API needs.

**Structure.** `kext/mps_xnu_os.cpp` is the only kext unit with core headers (inside
`extern "C"`, `MPS_COMPAT_NO_LIBC_REMAP`, after the IOKit/kern headers it needs, none of which
pulls in `<sys/queue.h>`; IOLib's `min`/`max` macros are undefined first). It implements the
`mps_os_*` hooks (table above) and `mps_kext_*` (`kext/mps_xnu_os.h`, plain C types; the softc
is opaque to the class). `SAS2308HBA.cpp` never sees a core header; the statistics struct moved
to `core/mps_xnu_stats.h` (only `<stdint.h>`) so the class can read a snapshot.
- Attach (`InitializeController`, the `Rig` order): `mps_kext_create` (`mps_compat_device_init`,
  `mps_bhandle` = BAR1) → `mps_kext_attach`: `mps_get_tunables`, debug mask, parent DMA tag,
  `mps_attach` (never `mps_free` after a failure: it frees itself), `xpt_register_async(AC_XNU_SCAN |
  AC_BUS_RESET | AC_SENT_BDR)` (AC_LOST_DEVICE until batch C, D36). `StartController`: `EnableInterrupt()`, then
  `mps_compat_intrhook_run(sc)` (mps_startup: unmask, port enable; this instance's hooks only, D40).
- IO: `ProcessParallelTask` maps the buffer (`gen64IOVMSegments`, ≤ `buffer_dmat` nsegments,
  ≤ 16 MiB − 1 per segment) into the `bus_dma_segment_t` list of an `XPT_SCSI_IO` ccb that lives in
  the task's HBA data (`ReportHBASpecificTaskDataSize` = ccb + path + 257 segments), LUN number,
  CDB, `timeout` 0, and calls `xpt_action` with mps_mtx held. Posted or held → `Request_In_Process`;
  completed inside `xpt_action` (unknown target, gone, invalid) → `FUNCTION_REJECTED` (invalid) or
  `SERVICE_DELIVERY_OR_TARGET_FAILURE`, as M4 did for refused IO. `cbfcnp` runs with mps_mtx held:
  it only queues the task (a leaf `IOLock` protects the done queue); `DeliverCompletions` runs
  without locks from the interrupt handler, `HandleTimeout`, the TM methods, the stop path, and
  from a thread call kicked when a callout or the reinit task completed ccbs.
- Completion mapping: `CAM_REQ_CMP` / `CAM_SCSI_STATUS_ERROR` → SCSI status byte, transfer count
  (`dxfer_len − resid`), autosense when `CAM_AUTOSNS_VALID`; `CAM_DEV_NOT_THERE`/`SEL_TIMEOUT`/
  `TID_INVALID`/`PATH_INVALID` → `DeviceNotPresent`; `CAM_CMD_TIMEOUT` → `TaskTimeoutOccurred` (D44; before: `ProtocolTimeoutOccurred`);
  `CAM_AUTOSENSE_FAIL` → CHECK CONDITION without sense (cam_periph_error: EIO, no retry; D20a);
  everything else (`CAM_SCSI_BUS_RESET` after the 4th reset in a row, `CAM_REQ_ABORTED` from an
  OS TM, transport errors) → `DeliveryFailure` (retryable); all non-status results with
  `SERVICE_DELIVERY_OR_TARGET_FAILURE`. Since batch A (see "FreeBSD deviation fixes, batch A"):
  IO killed by our reset is re-sent by the core (D20b), the UNIT ATTENTION after our reset is
  re-sent by the kext (D3), a firmware DEVICE_NOT_THERE also rescans the target (D20c).
- `HandleTimeout` → `mps_xnu_ccb_timeout` (held: completed at once; posted:
  `mpssas_scsiio_timeout`). The six TM methods → an `XPT_XNU_TM` ccb; the caller waits with mps_mtx
  dropped (`msleep` 10 ms steps) and pumps the reply queue (`mps_intr_locked`) between steps, as M4
  did, bounded by the core's TM timeouts. Abort: the class finds the task by target/LUN/tag; the
  glue re-checks task pointer + tag under mps_mtx; a task already completed is "complete".
  TM result: `CAM_REQ_CMP` → `FUNCTION_COMPLETE`; invalid request or IOC response NOT_SUPPORTED /
  INVALID_FRAME / INVALID_LUN → `FUNCTION_REJECTED`; else `SERVICE_DELIVERY_OR_TARGET_FAILURE`.
- Hotplug: the async callback samples `mps_xnu_target_present` for the scanned/lost target ids
  (all 64 for a wildcard scan) and the hotplug thread creates (present) or destroys (absent)
  targets. A reinit keeps the targets: handles are zeroed (FreeBSD `mpssas_handle_reinit`) and the
  wildcard rescan after the rediscovery's port enable destroys only the ones not found again (M4's
  `RediscoveryDone`). `InitializeTargetForID` asks `mps_xnu_target_present`.
- Stop: `fStopping` (no new `mps_kext_*` calls), `mps_kext_stop_waiters` (TM waiters leave; a TM
  ccb still in the core is freed when `mps_free` completes it), wait until no thread is inside the
  core, `DisableInterrupt`, `mps_free` (MUR, every ccb completed once with `CAM_DEV_NOT_THERE`, DMA
  freed), deliver, quiesce the hotplug and delivery thread calls. `TerminateController` does the
  same if `StopController` did not run, then frees the softc.
- Removed from the kext (the core does it now): the reinit thread (`RunReinit`/`ScheduleReinit`/
  `StopReinit`/`PostCoreLocked`/`HandleDeadLocked` → core taskqueue, `mps_xnu_disable` +
  `mps_os_controller_dead`), the `Service()` watchdog (→ `mps_periodic` and the TM/IO callouts;
  the `IOTimerEventSource` now only publishes statistics), SMID-indexed slots and the handle-keyed
  target table (→ the core's target ids), `MpsHw`/`MpsClient` (→ hooks + host callbacks).
- User-visible: properties, boot-args and `sas2308ctl` unchanged. `SAS2308Statistics`: same keys
  from `sc->xnu.stats` except "Reinits scheduled" (gone: the core queues reinits itself) and the new
  "Held total". A `SAS2308DebugMask` string goes through FreeBSD `mps_parse_debug` (an unknown name
  clears the mask, as on FreeBSD, instead of `kIOReturnBadArgument`). After a diag reset the kext
  turns memory decode and bus mastering back on (`mps_pci_restore`), which M4 did not do.
- Build: the core C files are compiled with the kext flags plus `-Wno-sign-compare`; Apple clang's
  `-mkernel` also enables `-Wsign-conversion`, `-Wshorten-64-to-32`, `-Wimplicit-int-conversion`
  and `-Wconditional-uninitialized` (126 + 10 + 7 + 3 hits, all in FreeBSD code that relies on
  implicit conversions or on a loop running once), which FreeBSD's build does not use: they are
  off for the core only. Two real `-Wformat` hits (`%jx` with `uint64_t`: `unsigned long long` vs
  `uintmax_t` = `unsigned long` on xnu) were fixed with casts. `compat/mps_compat.h` no longer
  defines `min`/`max` under `KERNEL` (Kernel.framework's `libkern.h` has the same); the
  freestanding stand-in `test/kernhdr/libkern/libkern.h` now declares them too, so
  `make -C test freestanding` would have caught it.

**All deviations from FreeBSD** (every one marked `XNU` in the code; 172 lines):
- *Platform*: compat headers instead of `<sys/...>`/`<cam/...>`; tunable defaults instead of
  `TUNABLE_*_FETCH`; one MSI vector; no sysctl tree, no shutdown/IR handlers, no `mps_user.c`
  ioctls/uio commands, no `%b` (`MPS_XNU_BITS`), lines keep their debug class (os_log category),
  field dumps are their own lines; `uintmax_t` casts; `mps_parse_debug` not static.
- *Not ported*: config pages and `mps_mapping.c` (simple target ids in `mps_xnu.c`, SAS Device
  Page 0 not read), EEDP, TLR, WarpDrive, SATA identify/START STOP UNIT, IR events, enclosure
  mapping, SMP passthrough, `XPT_PATH_INQ`/transfer settings/geometry.
- *No panic*: `KASSERT`/`panic` log a fault; failed reinit → controller disabled
  (`mps_xnu_disable`: gone, bus mastering off, everything failed `CAM_DEV_NOT_THERE`, wildcard
  rescan); `mps_reinit` runs on a taskqueue (never in callout context), retries the diag reset on a
  nested fault (≤ 3 attempts), every wait drops mps_mtx (`DELAY` ≥ 1 ms included).
- *Removal / shutdown*: all-ones register reads = surprise removal (`MPS_FLAGS_XNU_GONE`), checked
  by every wait loop; `MPS_FLAGS_SHUTDOWN` abandons a sleeping reinit; `mps_free` stops the IOC
  (MUR) and completes every command and held ccb before detaching; `mps_attach` frees on every
  failure, once.
- *Robustness*: reply-descriptor hardening (SMID range, reply address range/alignment, idle SMID
  counted), doorbell FAULT check when an interrupt finds nothing (M2–M4), event acks for
  AckRequired events, chain sizing guards (`num_chains` ≥ 1, frames ≥ 128 bytes, segments ≤ chain
  depth), unaligned TM LUN store and an `int` shift overflow (UBSan), `cdb_len` bounded before the
  CDB copy (FreeBSD's CAM does it; the thin CAM does not — static analyzer finding).
- *Recovery*: reinit requests are queued (`mps_xnu_request_reinit`), new IO requeued while one is
  pending; `mpssas_prepare_for_tm` freezes the whole target once per TM, released by
  `mpssas_free_tm`; an escalation reuses the TM's ccb; a timed-out TM stays owned until the reinit
  (late reply counted); recoveries without a free TM wait for one; no TM into a reset.
- *OS interface*: `XPT_XNU_TM` (six TM types, asynchronous; removed in batch B, D10), `mps_xnu_ccb_timeout` (OS timer,
  `timeout` 0 = no core callout; batch B, D15: 10 min except FORMAT UNIT / SANITIZE), `AC_XNU_SCAN` for rescans, `mps_xnu_target_present`.
- *Observability*: M4 statistics, trace lines, rate-limited decoded failed-IO line, TM line,
  periodic summaries (`mps_xnu.c`).

**Tests** (Linux g++ 13.3 ASan/UBSan/LSan and Mac Apple clang 21 ASan/UBSan, both PASS):
`mock_ioc_test` 38 scenarios, 406 CHECK sites (+ 39 in `mock_ioc.h`), 8070 SGLs walked, 122086
log lines checked, every ccb completed exactly once at detach; `mps_port_test` 10 scenarios, 141
checks. New in M5a-3: a CDB longer than `cdb_bytes` is refused with `CAM_REQ_INVALID`.
Changed expectations: those of M5a-2 (above); none new. The kext glue itself (thread calls,
IOKit DMA, the family) is compiled and link-checked only.

**`tools/diff-vs-freebsd.sh`** (common = lines unchanged; +/- = `diff -u` lines):
- Before (M4, measured at the start of M5): `MpsCore.{h,cpp}` vs FreeBSD `mpsvar.h` + `mps_sas.h` +
  `mps.c` + `mps_sas.c` + `mps_sas_lsi.c`: 33 lines shared of ~8000, +2667/−7951; `MpsDecode` vs
  `mps_table`: 17 lines shared, +823/−571.
- After (M5a-3): 6475 of 7244 lines unchanged from FreeBSD's 9489 (+769/−3014; the removed lines
  are mostly code not ported: mapping, EEDP, WarpDrive, IR, user ioctls, SMP). `mps_table.{c,h}`:
  475 of 490 lines unchanged, +15/−124. Per file:

| file | freebsd | ours | common | added | removed | xnu |
|---|---|---|---|---|---|---|
| mpsvar.h | 836 | 800 | 759 | 41 | 77 | 19 |
| mps_sas.h | 170 | 168 | 166 | 2 | 4 | 1 |
| mps_table.h | 66 | 62 | 62 | 0 | 4 | 0 |
| mps.c | 3232 | 2862 | 2614 | 248 | 618 | 63 |
| mps_sas.c | 3366 | 2564 | 2123 | 441 | 1243 | 76 |
| mps_sas_lsi.c | 1286 | 360 | 338 | 22 | 948 | 6 |
| mps_table.c | 533 | 428 | 413 | 15 | 120 | 7 |
| total | 9489 | 7244 | 6475 | 769 | 3014 | 172 |

## M5b: Mac conventions — kext layout, power management, termination, properties
Goal (user, verbatim): "try to make the code look as similar as existing code as possible (the
reference driver, and open source mac drivers) so that we have an easier time reviewing it. Also
make it compatible with standard mac driver stuffs". Research: `../mac-conventions-research.md`
(IOSCSIParallelFamily-352, IOPCIFamily, xnu PM, iSCSIInitiator, IntelMausi, itlwm, Linux mpt3sas).

### Kext layout
| File | Contents | Model |
|---|---|---|
| `SAS2308HBA.h` | the class; per-file sections; registry keys; PM states | IntelMausi.h |
| `SAS2308HBA.cpp` | core callbacks (`mps_pci.c` counterparts), the gate, completion delivery, `InitializeController` / `StartController` / `StopController` / `TerminateController`, `willTerminate`, `free`, capabilities, `ReportHBAConstraints`, DMA spec, interrupts, `ProcessParallelTask`, `HandleTimeout`, the six TM requests | IOSCSIParallelInterfaceController.cpp order |
| `SAS2308HBA_PM.cpp` | `InitializePowerManagement`, `setPowerState`, `systemWillShutdown`, quiesce / resume | IntelMausi `setPowerState`/`systemWillShutdown`, itlwm `pm.cpp` |
| `SAS2308HBA_Hotplug.cpp` | CAM async → target table, hotplug thread, `CopyTargetProperties`, `InitializeTargetForID`, per-target Protocol Characteristics, `TerminateTargets` | iSCSIVirtualHBA.cpp:150-162 |
| `SAS2308HBA_Props.cpp` | `Klog`, log settings, `setProperties` (sas2308ctl), `PublishControllerCharacteristics`, statistics timer and dictionary | — |
| `SAS2308HBA_Targets.h` | `SAS2308TargetTable`: created / wanted / busy bit masks, `Update`, `Next`, `Done`, `Stop`, `Leftovers` | — (plain C++) |

Method headers use the family's banner comments (`//---- Name - what it does. [PROTECTED]`) and
`#pragma mark` sections; members `fName`, constants `kName`. `start`/`stop` are not overridden: the
family header says they "shall not be overridden by the HBA child classes" (they are private);
PM is started from `StartController` and stopped from `StopController`/`TerminateController`/`free`.

### Power management (sleep / wake)
The family controller has no PM (only its target nubs: `IOSCSIParallelInterfaceDevice.cpp:480`).
- `InitializePowerManagement` (in `StartController`, before any target exists, so the nubs join
  the power plane below the controller): `PMinit`, `provider->joinPMtree(this)`,
  `registerPowerDriver` with two states, `changePowerStateTo(on)` so the controller never idles off
  (it must hear hotplug events). States: off = all zero; on = `kIOPMPowerOn | kIOPMDeviceUsable`,
  output and input `kIOPMPowerOn` (IntelMausi's array). No doze state: the IOPCIDevice's doze
  (`kIOPMSoftSleep`, Intel dark wake) maps to our off, the safe choice. No `IOPCIPauseCompatible`
  (a PCI pause then goes through off as well; pause.rtf).
- `setPowerState(off)` → `QuiesceController(10 s)` → `mps_xnu_suspend` (core/mps.c, XNU-only; Linux
  `scsih_suspend` shape, FreeBSD `mps_pci_suspend` returns EINVAL): `xnu.suspending` = new SCSI IO
  requeued (thin CAM holds it, SIM queue frozen), OS TMs `CAM_BUSY`; drain up to `drain_ms` while
  pumping the reply queue (outstanding commands, a pending/running reinit, queued firmware
  events); then stop `mps_periodic`, set `MPS_FLAGS_DIAGRESET`, mask interrupts, message unit
  reset (`mps_transition_ready`), complete what the IOC still owned like a diag reset
  (`mpssas_xnu_suspend` → `mpssas_complete_all_commands`: IO `CAM_REQUEUE_REQ`, held and re-sent
  after the wake, at most 3 times per ccb (D20b; before: `CAM_SCSI_BUS_RESET` → `DeliveryFailure`
  for READ/WRITE); TMs `CAM_REQ_ABORTED`), set
  `MPS_FLAGS_XNU_SUSPENDED`. While suspended `mps_regread`/`mps_regwrite` refuse every register
  access (counted `suspended_access`, logged once: a bug indicator), `MPS_XNU_ABANDON` is true, so
  no wait loop touches the (possibly powered-off) device. The kext then disables the interrupt
  source. Targets keep their nubs. The target nubs are our power children, so the disks have been
  synced/stopped through us before this runs.
- `setPowerState(on)` → `ResumeController` → interrupt source on, `mps_xnu_resume`: flags cleared,
  `mps_periodic` restarted, `mps_xnu_request_reinit("resume")` — the reinit runs on the core's
  taskqueue with FreeBSD's attach sequence first (D12, batch B: `mps_transition_ready` leaves the
  READY IOC our suspend's MUR left alone, then IOCFacts, IOCInit, events, port enable =
  rediscovery; no diag reset, so no PHY reset of the disks and no UNIT ATTENTION 29/00), the diag
  reset only if that fails (Linux `scsih_resume` likewise tries a MUR before a diag reset). Each
  wake logs `resume: controller reinitialized without a reset` or `... with a diag reset` and
  counts it (`Resumes without reset`, `Resumes with diag reset`); `setPowerState` returns
  `kIOPMAckImplied` at once. Held IO
  goes to the IOC when the rediscovery releases the SIM queue; targets found again keep their IDs
  (`mps_xnu_get_tid` prefers the last handle) and nubs; a target not found is destroyed by the
  wildcard rescan; a controller that disappeared while asleep is disabled by the reinit (held IO
  `CAM_DEV_NOT_THERE`).
- Edge: a reinit still running after the drain bound (diag reset > 10 s) gives up at its next wait
  once `MPS_FLAGS_XNU_SUSPENDED` is set; the resume reinit starts over.
- Edge (D49): an IOC that does not stop at the MUR is still marked suspended (bus mastering off,
  fault line `suspend: the IOC did not stop`, then `suspended without a clean stop ... the resume
  reinitializes the IOC`, counted `Sleep stop failures`): the wake's reinit, diag reset as the
  fallback, is what recovers it. A wake whose resume cannot run at all is a fault line (`wake:
  the controller could not be resumed`).
- Synchronous on purpose (D49): drain + MUR + reinit measured well under a second (498 ms wake),
  the worst case is bounded (10 s drain + the stop) and inside the PM thread's tolerance; an
  asynchronous ack (`acknowledgeSetPowerState`) would add a second state machine for no measured
  benefit.

### Shutdown / restart
The dock has its own PSU: across a Mac restart the IOC would keep running and could DMA into
memory the next kernel owns. `systemWillShutdown(kIOMessageSystemWillRestart | …WillPowerOff)`:
`fShutdown`, `QuiesceController(2 s)` (the same stop: drain, MUR), then `super`. The IOC stays
stopped (`setPowerState(on)` does not resume after a shutdown). FreeBSD has only an IR
`shutdown_final` handler; Linux `scsih_shutdown` also stops the IOC. From the start of the halt (priority
power interest, before any `systemWillShutdown`) every command has at most 10 s left and a reinit
is that stop (batch F below). `mps_free` works on a
suspended controller (registers left alone, held IO failed `CAM_DEV_NOT_THERE`).

### Termination
- `willTerminate` (before `super`) → `SAS2308Glue::WillTerminate`: `mps_kext_check_present` reads
  the doorbell, so a Thunderbolt surprise removal (all-ones) is noticed now. Noticing only queues
  the reinit (`mps_xnu_gone`), so `mps_kext_disable_now` waits for a running reinit (every wait of
  it checks GONE) and calls `mps_xnu_disable` synchronously: every posted, held and TM ccb completes
  `CAM_DEV_NOT_THERE`, and the completions are delivered before any nub goes (FreeBSD: `mps_free`
  completes every ccb before the SIM and its periphs go); then every target gets the M7 flush and
  drain (D5; before, the IO completed later on destroyed nubs). `fTargets.Stop()` (no new hotplug
  work); `QuiesceHotplug` waits for a `CreateTargetForID` in progress (its probe IO still completes,
  or fails fast when gone), so the family sees every created nub. Then
  `IOSCSIParallelInterfaceController::willTerminate` (stops new requests, destroys IDs
  `0 .. fHighestSupportedDeviceID − 1`), then `TerminateTargets` destroys what that loop missed:
  ID 63 (`SAS2308TargetTable::Leftovers`).
- The hotplug thread never creates a nub once `isInactive()`; the table hands out no work after
  `Stop()`; an `Update` during a create/destroy in progress is applied after it (a device lost
  while its nub is being created is destroyed afterwards, and the reverse).
- `StopController`, `TerminateController`, `free` and `ReleaseResources` are idempotent (every
  step guarded, pointers cleared); `free` covers a failed start.

### Properties published
| Where | Key | Value |
|---|---|---|
| personality (Info.plist), as AppleLSIFusionSAS | `Physical Interconnect`, `Physical Interconnect Location`, `Read Time Out Duration`, `Write Time Out Duration`, `IOPCITunnelCompatible` | SAS, External, 30000, 30000, true |
| HBA `Protocol Characteristics` | the family copies Interconnect/Location from the personality | SAS / External |
| HBA `Controller Characteristics` (`SetHBAProperty`) | Vendor Name, Product Name, Product Revision Level, Port Speed, Port Description | LSI, SAS9207-8i (subsystem 1000:3020; 3040 → -8e, else SAS2308), IOCFacts firmware "20.00.07.00", "6 Gigabit", "8-port SAS2 HBA, MPI 2.0, Thunderbolt" |
| target nub (`CreateTargetForID(id, dict)`) | `SAS Address` (8 bytes, big-endian, from SAS Device Page 0) | the family also sets `Unique SCSI Port Identifier` and names the nub `SAS Target <address>` |
| target nub `Protocol Characteristics` (from `InitializeTargetForID`, before registration) | Physical Interconnect, Location, Read/Write Time Out Duration | SAS, the HBA's Location, the HBA's timeouts |
| HBA | `SAS2308Statistics` (+ Suspends, Resumes, Suspend drain left, Config page reads/failures; State "suspended") | |
| HBA | `Controller Limits` (queue-freebsd) | IOCFacts RequestCredit, HighPriorityCredit, MaxReplyDescriptorPostQueueDepth, MaxChainDepth, IOCRequestFrameSize (bytes), ReplyFrameSize (bytes), MaxTargets, MaxVolumes, MaxMSIxVectors; effective Request / High-priority / Reply frames, Reply post queue depth, Chain frames, Max segments per IO, Core OS commands, OS task count; DMA / Heap / Task data bytes (Heap: the core's only since batch D, D45). At attach and after every reinit or resume |
| HBA | `PCIe Link` (batch D) | Link Speed / Link Width (Link Status), Max Link Speed / Max Link Width (Link Capabilities), Max Payload Size, Max Read Request Size (Device Control), Max Payload Size Supported (Device Capabilities), the four raw registers, BAR1 Posted Writes. With `Controller Limits` |
Not published: the HBA's own SAS address (SetHBAProperty in IOSCSIParallelFamily-352 accepts no SAS
key; it would need SAS IO Unit Page 0, not ported); `IOPCIUseDeviceMapper` (Apple's plist has it,
the DART is always used on Apple silicon).

### SAS Device Page 0 (core/mps_config.c)
`mps_config_get_sas_device_pg0` is FreeBSD's, verbatim (since batch C, D6: the page is
`malloc`'d and compat's `bus_dmamap_load` maps it for the IOC; until then one shared 1 KiB
`bus_dmamem_alloc` buffer was lent to each command, and a second reader got ENOMEM).
`bus_dmamap_load` cuts the segment list to `buflen`. `mpssas_add_device` reads the device's page and its parent's as FreeBSD does
(SASAddress, DeviceInfo, DeviceName, enclosure, slot, parent address/devinfo); a device page read
that the firmware fails skips the device (FreeBSD); one that fails on our side is retried (M5c). Still not ported: SATA identify (`mpssas_get_sas_address_for_sata_disk`,
so a SATA disk keeps the IOC's made-up SAS address) and `mps_mapping.c` (target IDs from
`mps_xnu_get_tid`). FreeBSD's `mps_push_sge` puts the page SGE in a chain frame (the 12-byte
`PageBufferSGE` holds a chain element); the mock accepts that, as the firmware does for FreeBSD.
`mps_config.c` vs FreeBSD: 169 of 183 lines unchanged (1320 removed: the other config pages).

### Exported symbols
Before M5b the kext exported the compat layer's FreeBSD names (`_msleep`, `_wakeup`, `_pause`,
`_DELAY`, `_getmicrotime`, …), which are also xnu BSD KPI names. Now the core, the compat layer
and `mps_xnu_os.cpp` are compiled with `-fvisibility=hidden`, the class files with
`-fvisibility-inlines-hidden`; `make check` lists `nm -gU` and fails on anything but
`__ZN(K)10SAS2308HBA…`, `__ZTV(N)10SAS2308HBA…` and `_kmod_info`. Result on the Mac: 85 exported
symbols, all the class's (methods, metaclass, vtables) and `_kmod_info`.

### Other changes
- os_log subsystem = the bundle ID `org.sas2308.driver.SAS2308HBA` (Makefile passes
  `SAS2308_BUNDLE_ID`); docs updated.
- `IOPCITunnelled` is read and logged at attach and shown in the Port Description.
- compat `msleep` returns the core's `EWOULDBLOCK` whatever the hook returns (the Linux host hook
  returned 11, the core's value is 35: `mps_wait_command` timeouts never reached their reinit
  path in the Linux host tests; xnu uses 35 for both).
- `ReportHBAConstraints` comment corrected: only `kIOMinimumHBADataAlignmentMaskKey` is
  dereferenced unconditionally by the family (`.cpp:1157`); the other keys have defaults.
- `make analyze` (clang static analyzer on every kext unit and the core).

### Tests (M5b)
- `mock_ioc_test` (+12 scenarios, 498 CHECK sites): config — page 0 for 2 devices and their
  parent (4 header + 4 page reads, chained SGE, SAS address/parent/devinfo/devname, `mps_xnu_target_info`),
  hot-added device, page read failure (device skipped), parent failure, lost config reply
  (`mps_wait_command` timeout → reinit → device found); sleep — idle suspend/resume (MUR, mask,
  no register access over 5 s, diag reset + port enable on resume, same target IDs), IO completing
  during the drain, IO still in flight after the drain (`CAM_SCSI_BUS_RESET`, retry + new IO held,
  held IO timing out, TM `CAM_BUSY`, all posted after wake), reinit pending at suspend, TM in
  flight at suspend, device removed while asleep (held IO `CAM_DEV_NOT_THERE`), shutdown = suspend
  then detach without resume.
- `kext_targets_test` (new, 113 checks): nothing before start, all 64 IDs, termination 0..63
  (63 via `Leftovers`), only 63, removal/re-add/failed create, lost during create, back during
  destroy, lost-and-back during create, hotplug thread racing termination (create of 5 and of 63
  in progress at `Stop`), stop with pending wishes.
- Not host-testable (IOKit only, hardware plan below): `setPowerState` sequencing with the PM
  tree, `systemWillShutdown` delivery, `willTerminate` ordering against the family, property
  publication, `PublishTargetCharacteristics` timing before `registerService`.

### Verified (2026-11-19, after M5b)
- Linux (g++ 13.3, ASan/UBSan/LSan): `mock_ioc_test` PASS (8070 SGLs, 128390 log lines),
  `mps_port_test` PASS (141 checks), `kext_targets_test` PASS (113 checks); `make freestanding` clean.
- Mac (Apple clang 21, ASan/UBSan): the same three PASS, no sanitizer reports, no build warnings.
- Kext build (CLT SDK 27.2, arm64e): rc 0, no compiler warnings (only the two known libkmod
  "built for 27.2" linker notes). `make check` rc 0: `plutil -lint` OK, signature OK,
  `kmutil libraries --undef-symbols`: all 391 undefined symbols resolve (= `nm -u`), public KPIs
  only: kpi.iokit 244, kpi.libkern 85, IOSCSIParallelFamily 46, kpi.mach 13, IOPCIFamily 3; nothing
  from kpi.private/unsupported. Exports: 85, class + `_kmod_info` only (no FreeBSD names).
- `make analyze` (default checkers + security.ArrayBound, alpha.unix.cstring) on the 5 kext units
  and the 8 core files: fixed one (a leaked `copyCollection` result when it is not a dictionary,
  `PublishTargetCharacteristics`); left 3, all in FreeBSD code: the known `mps_transition_operational`
  dead store and `mpssas_scsiio_complete` NULL `rep` (see M5a-3), and `mps_config.c:177` `cm` NULL
  (false positive: `mps_wait_command` clears `cm` only together with a non-zero error, which jumps
  to `out` first).
- NOT verified: anything at runtime in the kernel (PM callbacks, shutdown, termination order,
  properties as seen by ioreg/System Information).

## M5c: hot-plug page 0 failures, recovered-sense noise, IOC temperature (2026-11-20)

### Bug (hardware, 7 SATA Exos 18 TB, two SFF-8087 breakouts)
At the first plug-in 3 of 7 drives were not added; replugging the port-0 breakout (PHYs 0, 2, 3,
handles 0x9, 0xb, 0xc) re-added 0x9 only: `[fault] Error reading SAS device 0xb page0, iocstatus=
0x0`, `failed to add device with handle 0xb`, `invalid handle 0xb`; "Config page reads 10, Config
page failures 4". iocstatus 0 and the read counter (counted only once the page buffer was taken)
put the failure on our side, between the header read and the page read.

### Cause
`log show` of that minute: `mpssas_fw_work` ran on three kernel threads at once (event counter
62..67 interleaved on threads 0x26048, 0x25fbc, 0x26009). The IT firmware posts one topology change
list per PHY; each `mpssas_evt_handler` → `taskqueue_enqueue` → `thread_call_enter`. On xnu a
thread call entered while it runs starts a second invocation on another worker thread, and
`mps_compat_taskqueue_run` dropped the queue lock while a task ran, so a second
`mpssas_firmware_event_work` took the next event while the first slept (mps_mtx dropped) in a page
0 read. The second read got its header, then found the single config page buffer busy
(`mps_xnu_config_page` → NULL, `page alloc failed`, ENOMEM, `printf` = os_log DEBUG, not
persisted), and the device was dropped. FreeBSD runs each mps queue on one thread
(`taskqueue_start_threads(&sassc->ev_tq, 1, ...)`), so it never overlaps events. The same applied to
the reinit queue. Not involved: msleep/wakeup (IOLockSleepDeadline/IOLockWakeup are correct),
command allocation (64 IO + 8 internal commands), CAN_SLEEP.

### Fix
- `core/compat/mps_compat.c` `mps_compat_taskqueue_run`: `tq_active` — an invocation that finds
  the queue being run returns at once; the running one's loop picks the task up (FreeBSD's one
  thread per queue). The kext's thread call needs no change.
- Defensive (FreeBSD would fail the same way when every command is taken, e.g. 64 IO + pending
  event acks): `mps_xnu_get_sas_device_pg0` retries a page 0 read that failed on our side (EBUSY no
  command, ENOMEM buffer busy) or that the IOC refused for now (BUSY, INSUFFICIENT_RESOURCES) 3
  times, 10/20 ms apart (msleep, mps_mtx dropped); then `mps_xnu_readd_schedule` keeps the handle
  (16 slots) and the 1 s periodic queues `MPS_XNU_EVENT_READD` on the event queue 1, 2, 3, 4, 5, 5, 5,
  5 s later (8 attempts). Cancelled by TARG_NOT_RESPONDING, a reinit (`mpssas_handle_reinit`: the
  rediscovery adds it), a successful add or a target already present. Every schedule and give-up is
  a `fault` line with errno and reason; wait errors (timeout → reinit, reset in progress) and
  firmware errors (invalid page: handle gone) are not retried, as on FreeBSD. Counters: "Config page
  retries", "Device re-adds scheduled/given up". "Config page reads" now counts page 0 reads
  attempted (device and parent).
- FreeBSD `printf`/`device_printf` lines (class 0) are os_log DEFAULT instead of DEBUG.

### Recovered sense
macOS SMART polling sends ATA PASS-THROUGH(12) with CK_COND; SAT returns the ATA registers as
CHECK CONDITION, descriptor sense, RECOVERED ERROR, 0x00/0x1D (ATA PASS THROUGH INFORMATION
AVAILABLE). `mps_xnu_io_done`: CHECK CONDITION with sense key RECOVERED ERROR is not an error — not
in "Err check condition", counted in "Recovered sense (not errors)", the line says "recovered" and is
`xinfo` (silent at the default level), outside the failed-IO rate limit. The completion to the
family is unchanged (status + sense, as SAT requires).

### IOC temperature (IO Unit Page 7)
`mps_config_get_iounit_pg7` (core/mps_config.c, written like FreeBSD's `mps_config_get_ioc_pg8`, a
non-extended page, the same lent page buffer). `mps_xnu_periodic` (1 s callout, mps_mtx held) queues
`MPS_XNU_EVENT_TEMP` on the firmware event queue every 30 s: never before port enable completed
(`MPSSAS_IN_STARTUP`), never while firmware events wait (discovery first), never twice, never while
reset/reinit pending/suspending/suspended/gone/shutting down (the read re-checks when it runs). On
the event queue it cannot overlap a page 0 read. `IOCTemperatureUnits`: Celsius as is, Fahrenheit
converted, NOT_PRESENT → one info line and no more polling. One `info` line on the first reading,
an `error` line at ≥ 85 C (no warn class in mps_dprint), re-armed below 80 C. Published as the
controller property `IOC Temperature (C)` and in `SAS2308Statistics` ("IOC temperature (C)", reads,
read failures, warnings; `sas2308ctl stats` prints the dictionary). No macOS thermal API for
third-party HBAs is used.

### Tests (M5c)
- Host model: a queue kicked while it runs gets a nested invocation (xnu thread_call semantics)
  in every scenario; `queueReentries` counts them.
- `mock_ioc_test` hot-plug: the mock posts the next per-PHY topology list when a page read arrives
  (`topologyOnConfig`), IO held in flight to the other port: 3 drives on one port; the port
  unplugged and replugged as one 3-entry list while a new drive appears; 7 drives at port enable
  (one list per PHY). Without the compat fix the first fails with exactly the hardware signature
  (`page alloc failed`, `Error reading SAS device 0xc page0, iocstatus= 0x0`, `invalid handle`).
- Retry: pool exhausted (in-line retries, re-add 1/8 and 2/8, added once commands are free); device
  gone before the re-add; reinit cancels and the rediscovery adds; exhausted after 8 (fault line,
  nothing leaked); firmware invalid page not retried.
- Temperature: 30 s period, first-read line, 85/80 hysteresis, Fahrenheit, no sensor, nothing while
  suspended, due during a hot-plug page read → deferred while events wait, run after the page read.
- Recovered sense: ATA PASS-THROUGH(12) and (16) → xinfo "recovered", no error lines, not counted
  as check conditions, silent at the info level; TEST UNIT READY CHECK CONDITION still an error.

### Verified (2026-11-20, after M5c)
- Linux (g++ 13.3, ASan/UBSan/LSan): `mock_ioc_test` PASS (589 CHECK sites, 8070 SGLs, 133215 log
  lines), `mps_port_test` PASS (141 checks), `kext_targets_test` PASS (113); `make freestanding` clean.
- Mac (Apple clang, ASan/UBSan): the same three PASS. Kext build rc 0 (only the two known libkmod
  notes), `make check` rc 0: 392 undefined symbols resolve, public KPIs only, 85 exports.
- NOT verified: on the hardware (next: replug a breakout under IO, check all drives come back,
  "Config page failures" 0, `ioreg -l | grep "IOC Temperature"`).

## Review fixes (branch review-fixes, 2026-11-20)
Driven by the review in `docs/reviews/2026-11-final.md` (the ids below are its ids: "review C1"). FreeBSD behaviour preferred;
deviations marked `XNU:` in the code. Every fix has a host scenario that failed on the code before
it (core: `REVIEW_BEFORE` build of `mock_ioc_test`, `MOCK_ONLY=<id>`; kext: the behaviour-preserving
glue of the first commit, `KGLUE_ONLY=<id>`) and passes after.

### Fixed
| id | fix | host test (failed before → passes) |
|---|---|---|
| C1 | `ProcessParallelTask` calls `SetTimeoutForTask(task, 0)` after taking a slot, before the submit; from then on every outcome completes through `CompleteParallelTask` (the only call that removes the timer entry). The core's FreeBSD callout is armed as a backstop at task timeout + 10 s (`MPSK_BACKSTOP_MS`) | glue `C1` (held IO, timer fires, abort, `ProtocolTimeoutOccurred`), `C1-backstop` (family timer lost) |
| L1 | refusals inside `xpt_action` are queued on the done queue and delivered by the done thread call (no family re-entry from the submit path): `DeviceNotPresent` / `FUNCTION_REJECTED` survive | glue `L1` |
| C2 | FreeBSD's `mpssas_get_sas_address_for_sata_disk` / `mpssas_get_sata_identify` / `mpssas_ata_id_timeout` ported (IDENTIFY into the lent config page buffer instead of malloc); `mps_xnu_get_tid(sc, handle, ident)` keyed on the identity like `mps_mapping_get_tid` (SAS address, IDENTIFY hash for SATA; unknown = new disk, except in a reinit's rediscovery since D8); per-id generation (`mps_xnu_target_gen`) passed with every async event; the target table never keeps a nub across a generation change (destroy, then create, cursor stays on the id) | core `C2-swap`, `C2-permute`, `C2-sas`; glue `C2`; `kext_targets_test` generation case |
| H1 | hotplug thread call: `fHotplugActive`/`fHotplugAgain` (the compat taskqueue's `tq_active` pattern): a re-entered invocation makes the running one scan again | glue `H1` (removal during `CreateTargetForID`'s probe; the model's thread call runs the second invocation concurrently) |
| H2 | `mps_xnu_request_reinit` drops requests while `MPS_FLAGS_DIAGRESET` is set and not suspending (FreeBSD's "reset already in progress" covers them); `mps_reinit_task` returns when `reinit_pending` is 0 (an in-line reinit ran) | core `H2-inline`, `H2-task` (one diag reset, flag 0, IO flows) |
| H3 | `dmb oshst` before every MMIO store, `dmb oshld` after every MMIO load (`mps_os_bus_write_4/read_4`, arm64 only) | none possible on the host (FINAL: not observable); Mac build only |
| H4 | `mpssas_prepare_remove` rescans before allocating the TM; a removal without a free TM is flagged (`MPSSAS_TARGET_XNU_REMOVE_WAIT`) and started from `mpssas_free_tm` → `mpssas_xnu_retry_recovery`; TM pool 8 (then `MPS_XNU_PRI_REQ_FRAMES`; FreeBSD's 128 since queue-freebsd, IOCFacts HighPriorityCredit bounds it) | core `H4` (7 removals in one batch, 4 TMs: 7 rescans, 7 REMOVE_DEVICE) |
| H5 | `mps_xnu_suspend(sc, drain, shutdown=1)` from `systemWillShutdown`: drain ≤ 1 s, one MUR with a 1 s ack wait, no diag reset, stop bounded at 2 s (`stop_deadline_us`), bus mastering off if the IOC did not stop, then every command and held ccb failed and `xnu.shutdown` makes `mpssas_action_scsiio` fail new IO (`CAM_DEV_NOT_THERE`) instead of holding it; no resume afterwards | core `H5` (wedged IOC: < 5 s virtual, no diag reset, `controller_dead` first, new IO fails at once), `H5-retry` |
| M1 | the suspend asks a running reinit to give up (`stop_reinit` in `MPS_XNU_ABANDON`) and waits ≤ 2 s for it; a reinit abandoned while suspending neither disables the controller nor touches `MPS_FLAGS_DIAGRESET`, and keeps its discovery hold for the resume's reinit (`reinit_owed`) | core `M1` |
| M2 | `mps_wait_command` returns ENXIO when `SHUTDOWN`, `XNU_GONE` or `XNU_SUSPENDED` | core `M2` (detach with three adds queued: no 60 s waits) |
| M3 | `mps_iocfacts_allocate` no longer calls `mps_free` (once, in `mps_attach`); `mps_free` idempotent (`xnu.freed`); `mps_iocfacts_free` forgets what it freed; TF6 also found and fixed: the chain-load failure path freed the chain memory twice, unchecked `M_WAITOK` results (commands, event handles, sassc, targets), unchecked taskqueue create/start, `panic` on a failed dmamap create, `bus_dmamap_load` allocating for 1-segment loads whose callbacks read `segs[0]` (L14), the intrhook left on the global list after a failed attach | core `M3` (every one of 108 allocations of `mps_attach` failed in turn under ASan) |
| M4 | if the IOC does not stop (suspend, `mps_free`): `mps_os_controller_dead` before anything completes; both stops time-bounded (10 s) | core `M4` |
| M6 | IO killed by our own reset that is not READ/WRITE (SYNCHRONIZE CACHE, pass-through) is completed `CAM_REQUEUE_REQ` (held, re-sent after the reset), at most 3 times per ccb (`ccb_h.xnu_resets`); since D20b READ/WRITE too | core `M6` |
| M7 | before `DestroyTargetForID` the hotplug thread fails the target's held ccbs (`cam_xnu_flush_held_target`, `CAM_DEV_NOT_THERE`), waits ≤ 10 s for its posted IO (`mps_kext_target_drain`), delivers, then destroys; since D14 no time limit (a reinit after 60 s) | glue `M7` (fake family asserts no completion after destroy) |
| M8 | resume tolerates an all-ones doorbell for up to 2 s (raw reads, 100 ms apart) before `mps_periodic` may declare the controller gone | core `M8` |

### Test fidelity (FINAL section 4)
- TF1: `kext/SAS2308Glue.{h,cpp}` (slots, gate, `ProcessTask`, `HandleTimeout`, delivery, hotplug
  loop) and `kext/mps_kext.cpp` (the `mps_kext_*` calls, moved out of `mps_xnu_os.cpp`, only
  `mps_os_*` hooks) are compiled into `test/kext_glue_test` with the core and the IOC model; the
  fake family times only tasks given to `SetTimeoutForTask`, rejects a refused task still on the
  timer list, asserts no completion after `DestroyTargetForID`. `HostThreadCall` in
  `test/mps_host_os.h`: xnu `thread_call` semantics (enter while running = a concurrent, nested
  invocation; cancel only dequeues). The core's taskqueue keeps its equivalent model.
- TF2 (part): mock device identity: SATA devices answer `SATA_PASSTHROUGH` IDENTIFY with a serial
  number, SAS addresses per handle can change, IDENTIFY can fail; MUR can be ignored (wedged IOC);
  registers can read all-ones for a while (late tunnel).
- TF3 (part): lost-wakeup semantics: `msleep` forgets wakeups issued before it.
- TF4: virtual-time budgets in `H5`, `M4`, `M2`.
- TF6: allocation failure injection (`gHost.failAllocAt`, malloc and DMA allocations).

### Verified on the Linux host (Linux g++ 13.3, ASan/UBSan/LSan)
`make -C test test`: `mock_ioc_test` PASS (all earlier scenarios + 14 review scenarios + sizing, 8070 SGLs, 162767 log lines),
`mps_port_test` PASS (142 checks), `kext_targets_test` PASS (121 checks), `kext_glue_test` PASS
(8 scenarios, 2443 checks), `make freestanding` clean. Changed expectations: one test pins the TM
pool to 4 (more targets than TMs), queue sizing with the model's RequestCredit 100: 108 requests
/ 127 replies, 92 OS commands. The same tests pass on the Mac (Apple clang, ASan/UBSan), and the
kext builds there with `make`, `make check` and `make analyze` clean (analyze: 4 upstream FreeBSD
null-deref/dead-store false positives in mps.c/mps_config.c/mps_sas.c, as on master).

### Tuning after the 2026-11-20 benchmark (`docs/benchmarks-20261120.md`)
- **Command limit.** 64 commands for all drives cost ~21 % random IOPS on 7 drives and caused
  13-52 s waits above the driver. Now `mps_resize_queues`: normal frames
  `reqcr = min(MPS_XNU_REQ_FRAMES (256 + 8), RequestCredit, MaxReplyDescriptorPostQueueDepth - prireqcr - 1)`
  (FreeBSD `hw.mps.max_reqframes` sizing; the last bound is XNU: the post queue must hold a
  descriptor per frame), `xnu.io_cmds = reqcr - 8` (8 internal, as before: 72 frames = 64 IOs).
  Chains stay `29 × reqcr` (7656 frames, 957 KiB at 264), sense `32 × num_reqs`, reply frames
  `min(272 + 32, post queue depth) - 1` (FreeBSD: from the maximum, not the credit). The kext
  reports `ReportMaximumTaskCount = xnu.io_cmds` (`mps_kext_max_tasks`, `SAS2308Glue::Attached`;
  the family asks right after `InitializeController` and publishes `IOCommandPoolSize` = what it
  allocated); the glue has 256 task slots (`MPSK_MAX_TASKS`), `fMaxTasks` of them used. A reinit
  that resizes the core keeps the family's pool; IO beyond the core's commands waits in the thin
  CAM. Attach logs `RequestCredit .. HighPriorityCredit .. ReplyPostQueueDepth ..: N OS commands`.
  Tests: core `sizing` (5 credit/depth cases: pools, `io_cmds` IOs posted and completed at once,
  SMIDs within the credit), glue `tasks` (256 accepted, the 257th refused).
  Superseded by "Queue sizes as FreeBSD" below (no 256 cap).
- **Event 0xff02** is not an MPI2/LSI event (MPI2 codes end at 0x7F) but our own
  `MPS_XNU_EVENT_TEMP` work item (IOC temperature poll every 30 s; 0xff01 = `MPS_XNU_EVENT_READD`).
  Its "Working on / Event Free" lines are now xinfo, not the event class, and named
  (`XnuTemperaturePoll`, `XnuReAdd` in `mps_event_names`).

### Not done in this branch
M5 (4 GiB SGE split), L2-L17 except L1/L13 (partly)/L14, TF5 (barrier contract), TF7, TF8,
TF9 beyond H4/M2. H3 is only verifiable on hardware. The kext is compiled on the Mac only
(no Kernel.framework here); nothing of this branch has been loaded yet.

## Queue sizes as FreeBSD (branch queue-freebsd, 2026-11-20)
- **Sizing.** `mps_get_tunables` sets FreeBSD's defaults: `max_reqframes` = `MPS_REQ_FRAMES`
  2048, `max_prireqframes` = `MPS_PRI_REQ_FRAMES` 128, `max_replyframes` = `MPS_REPLY_FRAMES`
  2048, `max_evtframes` 32, `max_chains` 16384 (the `MPS_XNU_IO_CMDS_MAX` 256 cap and the
  `MPS_XNU_*_FRAMES` defines are gone). `mps_resize_queues` as FreeBSD (`min` with
  RequestCredit / HighPriorityCredit) plus two XNU lines: normal frames at most
  `MaxReplyDescriptorPostQueueDepth - prireqcr - 1`, and `num_replies =
  min(max(max_replyframes, num_reqs) + max_evtframes, MaxReplyDescriptorPostQueueDepth) - 1`,
  so the post queue holds a descriptor for every request frame (FreeBSD's own defaults give
  2176 frames but 2079 replies: a 2080-entry post queue). `xnu.io_cmds` = normal frames - 8
  internal commands = `ReportMaximumTaskCount`; the family allocates that many tasks
  (`IOCommandPoolSize`).
- **Kext.** `MPSK_MAX_TASKS` / `kMaxTasks` removed. The glue's task slots (32 bytes each) are
  allocated by `SAS2308Glue::Attached` for exactly `fMaxTasks` tasks (IOMalloc, attach context,
  may block; attach fails cleanly without them) and freed by `Fini`. Slot search stays linear
  (first free; only outstanding tasks are scanned in practice).
- **SAS9207-8e (firmware 20.00.07.00), "Controller Limits":** RequestCredit 10240,
  HighPriorityCredit 128, MaxReplyDescriptorPostQueueDepth 65504, MaxChainDepth 128,
  IOCRequestFrameSize 128 bytes, ReplyFrameSize 128 bytes, MaxTargets 1024, MaxVolumes 0,
  MaxMSIxVectors 0. Effective: 2048 request + 128 high-priority frames, 2207 reply frames,
  post queue 2208, 16384 chain frames, 257 segments per IO, 2040 OS tasks
  (`IOCommandPoolSize` 2040, was 256).
- **Memory** (published as DMA / Heap / Task data bytes): DMA 3,233,152 bytes (request frames
  2176 x 128 = 272 KiB, reply frames 2208 x 128 = 276 KiB, free + post queues 26 KiB, sense
  2176 x 252 = 536 KiB, chain frames 16384 x 128 = 2 MiB; 5 `bus_dmamem_alloc`s plus the 1 KiB
  config page buffer, each rounded to 16 KiB pages behind the DART); core heap
  + slots 1,233,664 bytes (commands, chain trackers, DMA maps, 2040 slots); the family's task
  data 9,481,920 bytes (2040 x 4648: the ccb plus 257 segments per task). About 13 MiB wired
  in all, of 16 GiB; at 256 tasks it was about 3 MiB.
- **Logging.** One info line at attach: `IOCFacts: RequestCredit .., HighPriorityCredit ..,
  MaxReplyDescriptorPostQueueDepth .., MaxChainDepth .., request frame .. bytes, MaxTargets ..;
  using .. request + .. high-priority frames, .. replies, .. chain frames, .. OS tasks; memory:
  ..`. Known: the kext's lines from early boot (attach, discovery) are not in the persisted log
  on this Mac (also before this branch); read `Controller Limits` instead, or boot with
  `sas2308_iolog=1`.
- **Tests.** Core `sizing`: 12 cases, RequestCredit 12 .. 10240 (2047 and 1500 keep the
  credit, 2049 and 10240 give 2048), HighPriorityCredit 8 and 200 (128 TMs), post queue
  depths 128 .. 65520 (1600 and 2096 clamp), `max_replyframes` tuned to 64; in each case
  `io_cmds` IOs are posted at once and completed at once (the model checks every post queue
  slot was recycled). Mutation check: FreeBSD's `num_replies` formula fails the tuned case with
  a post queue overrun. Glue `tasks`: RequestCredit 10240 → 2040 tasks accepted, the 2041st
  refused, `mps_kext_limits` values. All host tests pass on the Linux host (g++ ASan/UBSan) and on the
  Mac (Apple clang).
- **Hardware** (kext A42837FD, 7 SAS Exos, `docs/benchmarks-20261120.md` "Queue depth with 2040
  commands"): random 4K read 1748 / 1742 / 1738 IOPS at QD 32 / 64 / 128 per drive (896
  commands outstanding at the card), latency doubling with each step; sequential read 1941 MB/s;
  verify 0 mismatches; 0 timeouts, TMs, reinits, held or rejected tasks. Merged to master.

## FreeBSD deviation fixes, batch A (branch fix-batch-A, 2026-11-20)
The likely bugs and data-safety items of `docs/reviews/2026-11-freebsd-deviations.md` (final fixes
in `docs/reviews/2026-11-decisions.md`). Each host test failed on the code before its fix
(`MOCK_ONLY=<id>` / `KGLUE_ONLY=<id>` runs one).

| id | fix | host test |
|---|---|---|
| D1 | `MPSSAS_TARGET_XNU_REMOVE_WAIT` is bit 7 (was bit 6 = FreeBSD's `MPS_TARGET_IS_SATA_SSD`: the first freed TM removed a SATA SSD); static asserts: fits the `uint8_t` flags, overlaps no flag | core `D1` (SSD in the mock, TM freed) |
| D2 | FreeBSD's loop at the end of `mpssas_add_device` restored verbatim: a SATA PASSTHROUGH flagged `SATA_ID_TIMEOUT` gets a TARGET_RESET, the IOC returns it, `mpssas_ata_id_complete` frees the config page buffer (before: held until a reinit, every page read failed) | core `D2` |
| D3 | CAM da's RETRY_UA in the kext (`mps_kext_arm_ua`/`mps_kext_retry_ua`): AC_BUS_RESET arms every target, AC_SENT_BDR a target or LUN (with its generation); CHECK CONDITION 6/29/xx on an armed LUN is re-sent once per ccb, uncounted by SAM; the first GOOD from the LUN disarms it. SAM sends SYNCHRONIZE CACHE once and absorbs 29/00 only at power-on. da retries any key 6; we match ASC 0x29 only. Counter "Unit attentions after reset retried" | glue `D3` (SYNCHRONIZE CACHE GOOD after a reinit and a target reset; a UA without a reset of ours reaches the OS) |
| D5 | termination of a removed controller: `mps_kext_disable_now` before the family destroys the nubs ("Termination" above) | glue `D5` |
| D8 | a SATA IDENTIFY that fails in a reinit's rediscovery (spin-up after a wake) no longer makes a new disk: if the same handle had a known identity before the reset (`mps_xnu_reinit_targets`, `mps_xnu_ident_keep`), the id keeps it and its generation, its devq is frozen and a re-IDENTIFY runs on the re-add schedule (`MPS_XNU_EVENT_READD`, `reident` slot, 8 tries 1-5 s apart). Same disk: IO released. Different disk or no answer: held IO `CAM_DEV_NOT_THERE`, new generation, the OS replaces the nub. The made-up SAS address of a direct-attached SATA disk names the bay, not the disk (review C2), so it is not trusted. Any identity change also fails the id's held IO, and `mps_kext_submit` refuses (`DEV_NOT_THERE`) a task whose nub generation (`SAS2308TargetTable::Gen`) is not the core's: no IO of the old disk reaches a new one | core `D8`, `D8-swap`, `D8-giveup`; glue `D8` |
| D14 | the hotplug destroy waits for the target's posted IO with no time limit (CAM frees a periph when its last ccb returns); posted IO is never completed by us (M4 rule); after 60 s (`kDrainReinitMs`) a reinit is requested and the wait goes on. FreeBSD arms no timeout for the removal TM | glue `D14` (TM answers after 20 s), `D14-cap` (never: reinit at 60 s) |
| D20 | (a) `CAM_AUTOSENSE_FAIL` → CHECK CONDITION without sense, not retried; (b) READ/WRITE killed by our reset re-sent like the rest (M6), ≤ 3 times per ccb, uncounted by SAM; (c) a firmware DEVICE_NOT_THERE rescans the target (cam_periph_error's AC_LOST_DEVICE); (d) kept | glue `D20a`; core `D20b`, `D20c` |

Comments for KEEP items in the files touched: D28 (EEDP), D43 (drain pump), D46 (no ORDERED tags).
Hardware tests and their results: `docs/reviews/2026-11-freebsd-deviations.md` (Status).

Verified on the Mac (build DDA7E6AD, 2026-11-20): every wake from sleep makes each of the 7 drives
answer the first command with 29/00 (7 per wake, all re-sent); 5 sleep/wake cycles with
F_FULLFSYNC writers on two mounted JHFS+ volumes: 0 errors, no eject, 0 identity changes, volumes
verify OK. Random 4K QD32 1770 IOPS, seq 1 MiB 1945.5 MB/s, verify 0 mismatches.

## FreeBSD deviation fixes, batch B (branch fix-batch-B, 2026-11-20)
Timeouts, recovery, resume and stop items of `docs/reviews/2026-11-freebsd-deviations.md` (final
fixes in `docs/reviews/2026-11-decisions.md`). Each host test failed on the code before its fix
(`MOCK_ONLY=<id>` / `KGLUE_ONLY=<id>` runs one; D43's did not build).

| id | fix | host test |
|---|---|---|
| D4 | the family timer counts from the post to the IOC, as FreeBSD's callout: `HandleTimeout` re-arms (`SetTimeoutForTask(task, rest)`, never 0) a task still held or owned by the IOC (`qos.sim_data`) for less than its timeout; re-armed in the work-loop gate, before any completion of the task can reach the family. Counter `Family timer re-arms` | glue `D4` (held 25 s, then 10 s in the IOC: no TM; held then stuck: exactly one timeout 30 s after the post; a re-armed task is not expired again in the same timer loop) |
| D15 | timeout 0 (SAM's START STOP UNIT, pass-through) keeps no family timer, but the core's callout fires after 10 min, except FORMAT UNIT and SANITIZE; 30 s Read/Write Time Out kept (Linux sd default, the macOS SAS HBA value; FreeBSD da 60 s), reason in Info.plist | glue `D15` |
| D44 | a timed-out task completes `TaskTimeoutOccurred`, the family's own status for its timer | none (IOKit class) |
| D10 | OS task management as FreeBSD: LUN and target reset → `XPT_RESET_DEV` (`mpssas_action_resetdev`, with `mpssas_send_reset`'s 30 s TM timeout because the OS waits synchronously), held by the thin CAM behind a frozen device or SIM queue as CAM's devq does; abort of a posted task → its timeout recovery (`mps_xnu_ccb_timeout`, one TM per target), the request returns when the task is back; abort of a held task → completed `CAM_REQ_ABORTED`; ABORT TASK SET, CLEAR TASK SET, CLEAR ACA → FUNCTION_REJECTED. `XPT_XNU_TM`, `struct ccb_xnu_tm`, `mpssas_action_xnu_tm` deleted (review L6 gone) | glue `D10`; the mock's OS-TM scenarios now use `XPT_RESET_DEV` |
| D11 | an all-ones register read is confirmed before "gone": `mps_xnu_gone` arms a callout that re-reads the doorbell and asks whether the PCI config space answers (vendor id), 5 times 20 ms apart; gone only if every try fails, else `Transient all-ones reads` +1. `willTerminate` (`mps_kext_check_present`) and `mps_reinit` wait for the verdict | core `D11` (one bad read: no disable; unplug: disabled) |
| D12 | resume: FreeBSD's attach sequence (`mps_transition_ready`, IOCFacts, IOCInit, port enable), the diag reset as the fallback; logged and counted per wake (Power management above) | core `D12` (READY: no diag reset, same ids; IOCInit faults: diag reset) |
| D13 | `saveDeviceState(kIOPCIConfigShadowPermanent)` in `StartController` (after the interrupt source is enabled: MSI programmed; Permanent = save only, no power-state change, stays valid across restores); `HostPciRestore` calls `restoreDeviceState` plus the two command bits as before. PCI command, MSI control/address/data and restore counts are published with the statistics | none (IOKit class); hardware check below |
| D19 | `mps_pci_restore` after every diag reset of a reinit (was: the first only) | core `D19` |
| D18 | `mps_add_chain` / `mps_push_sge`: after the "Need SGE Error Code" panic line (our `panic()` returns) EFBIG instead of an SGE written into the next request frame; such a command is failed `CAM_REQ_TOO_BIG` (`MPS_CM_FLAGS_XNU_SGL_FAILED`), not requeued as a chain shortage | core `D18` (guard pattern in the next frame; segment cap = `max_io_pages` + 1) |
| D51 | `mps_free` polls the reply queue once (`mps_intr_locked`) after setting SHUTDOWN, before the MUR: IO the IOC completed whose interrupt was not delivered (ours is off before `mps_free`: the family owns the source, nothing like `bus_teardown_intr` waits for a running handler) completes with its real status. Order documented in `StopController` | core `D51` |
| D43 | pumps kept (FreeBSD's NO_SLEEP waits poll; completions are delivered in the work-loop gate), comments on both; the 1 s statistics timer delivers only when no done call is pending and counts what it delivers (`Lost completion kicks`, logged once) | glue `D43` |

Debug triggers for the hardware tests (`sas2308ctl reinit | hold <ms> | lose <n>`, properties
`SAS2308DebugReinit`, `SAS2308DebugHoldMs`, `SAS2308DebugLose`; admin only): a reinit (diag
reset), new IO held with the SIM queue frozen, the next n SCSI IOs marked posted but never given to
the IOC (one hook in `mps_enqueue_request`). Host test: glue `debug`.

The host sleep model now lets time pass in 1 ms steps during a long `msleep`, so callouts due
meanwhile run first, as on a real kernel (needed by D11's callout).

Verified on the Mac (build 9E6663B6, 2026-11-20; details in the deviations plan): 7 resumes under
IO all by the READY path (0 diag resets), but every drive still answers the first command after a
wake with 29/00 (the MUR / port enable resets the links), so D3 stays essential; MSI address/data
unchanged across a forced diag reset; a task held 35 s is re-armed, not timed out; a lost command
times out once and recovers through the whole escalation; unload under IO is clean. Random 4K QD32
1768 IOPS, seq 1 MiB 1945.4 MB/s, verify 0 mismatches.

## FreeBSD deviation fixes, batch C (branch fix-batch-C, 2026-11-20)
Compat fidelity items of `docs/reviews/2026-11-freebsd-deviations.md`: the core goes back to FreeBSD's
text, the difference moves into compat or the kext (final fixes in `docs/reviews/2026-11-decisions.md`).
Each host test failed on the code before its fix (`MOCK_ONLY=<id>` / `KGLUE_ONLY=<id>`), except
where noted. `tools/diff-vs-freebsd.sh -s`: 1265 added / 3954 removed lines before, 1198 / 3836 after.

| id | fix | host test |
|---|---|---|
| D37 | `cam_compat.h` has CAM's own `XPT_*` codes (with `XPT_FC_*`), `AC_ADVINFO_CHANGED` and `CAM_PRIORITY_NORMAL`; `xpt_action` holds by `XPT_FC_DEV_QUEUED` | core `D37` |
| D36 | the kext no longer listens for `AC_LOST_DEVICE` (nothing posts it; removals arrive as `AC_XNU_SCAN`) | glue `D36` |
| D27 | `%b` in compat (`mps_compat_vsnprintf`, FreeBSD subr_prf.c output: no `0x`); the 7 `%b` sites are verbatim, `MPS_XNU_BITS` gone; clang checks the formats as `freebsd_kprintf` | core `D27` (new API) |
| D16 | `DELAY` keeps the caller's locks (FreeBSD's spin; the handshake is atomic again); the mutex registry is gone | core `D16` |
| D35 | `xpt_done` marks the held-queue run, which happens once when the SIM's mutex is let go (`mtx_release_fn`, CAM's done thread); a frozen SIM stops a run at once, the rest stays held in order (merge) | core `D35` (2000 posted + 500 held reinit: 3.1 M held-ccb checks before, 2501 after); thin CAM test |
| D67 | `xpt_done` asserts that a requeue is backed by a freeze or releases one outside a run | core `D67` |
| D17 | `xpt_periph` is a sentinel: a freeze through the SIM's own path (the TM ccb) holds the whole target in compat; `mpssas_prepare_for_tm`'s path is FreeBSD's (tid, lun); the kext's completion callback says why retries reorder | core `D17` |
| D40 | `mps_compat_intrhook_run(arg)` runs only that instance's hooks | core `D40` |
| D42 | `bus_dmamap_load` errors as busdma: to the callback, returned only if ENOMEM | core `D42` |
| D41 | tags inherit the parent's boundary (the smaller one); `bus_dmamap_load_ccb` splits a segment at the boundary (4 GiB for `buffer_dmat`, FINAL M5), EFBIG -> `CAM_REQ_TOO_BIG` if the list gets too long (not a requeue: same list forever) | core `D41` |
| D39 | one kernel thread per taskqueue (`mps_compat_taskqueue_thread` = `taskqueue_thread_loop`, `kernel_thread_start`); drain sleeps, free waits for the thread to leave | core `D39` (new API) |
| D33 | `mps_get_tunables` verbatim; `TUNABLE_*_FETCH` read `mps.X` / `mps.N.X` from boot-args, then the personality (`mps_os_getenv`, `SAS2308HBA::HostGetenv`) | core `D33` |
| D32 | `MPS_MAXIO_PAGES` in the core; `mps.max_io_pages` 256 in Info.plist; the kext takes `sc->maxio` (`mps_kext_limits.maxio`) for `kIOMaximumByteCount*` and sizes each task's segment list from `buffer_dmat` (task data size asked after `InitializeController`) | core `D32` |
| D21 | `mps.c` sizing is FreeBSD's; `mps_kext_max_tasks` = normal frames - `MPSK_INTERNAL_CMDS` (8), with the reason in the kext | existing tests follow the move (same value) |
| D54 | `mpsvar.h`: `mps_reinit(sc)` queues the reinit task (`mps_xnu_request_reinit(sc, __func__)`), so FreeBSD's callout/completion call sites are verbatim; `(mps_reinit)(sc)` is the reset (task, `mps_wait_command`) | core `D54` |
| D78 | `compat/queue.h` has FreeBSD's `QUEUE_MACRO_DEBUG_TRASH` and `_ASSERTIONS`; the host build turns them on | core `D78` |
| D6 | `bus_dmamap_load` maps a core `malloc` block (`mps_os_dmamap_load_buf`: IOMemoryDescriptor + IODMACommand on the DART), released at unload, next load, `free` of the block (the malloc header records the map) and destroy; config pages and IDENTIFY use `malloc`/`free` verbatim; the lent buffer and `cfg_busy` are gone. The page 0 retries and re-add slots stay until the breakout-replug test | core `D6` (2 page reads during a stuck IDENTIFY: ENOMEM before; 1000 reads -> 0 mappings; free/reload release) |

Comments for KEEP items in the files touched: D38 (malloc may block), D65 (openings not enforced),
D69 (boot hold no-ops), D17 (`mps_kext_io_done`). Hardware tests and results: the deviations plan.

Verified on the Mac (build 6DD7CE42, 2026-11-20): all config pages read into `malloc`'d pages, 0
failures, no IODMACommand/descriptor growth across the temperature polls; a forced reinit and a lost
command under IO on all 7 drives recover with 0 errors (DELAY with the lock, the taskqueue thread, the
deferred held run); a `shutdown -r` at full write rate leaves every block with old or new data, none
corrupt (D34). Random 4K QD32 1765 IOPS, seq 1 MiB 1944.6 MB/s, verify 0 mismatches.

## FreeBSD deviation fixes, batch D (branch fix-batch-D, 2026-11-20)
Kext glue and IO path items of `docs/reviews/2026-11-freebsd-deviations.md` (final fixes in
`docs/reviews/2026-11-decisions.md`), the agreed counters, and the card's PCIe link in the registry.
Each host test failed before its fix (`MOCK_ONLY=<id>` / `KGLUE_ONLY=<id>`), except where noted.
`tools/diff-vs-freebsd.sh -s`: 1198 added / 3836 removed lines before, 1281 / 3595 after (common
7147 -> 7383: D9 and use_phynum bring FreeBSD code back; D31's handler and D7's hooks are new).

| id | fix | host test |
|---|---|---|
| D52 | the personality matches 0x0086, 0x0087 and 0x006E (FreeBSD's SAS2308 ids) | `test/plist_test.sh` |
| D48 | `kIOMinimumSegmentAlignmentByteCountKey` 1, as the DMA specification and FreeBSD's buffer tag | none (IOKit class) |
| D30 | no doorbell read on an interrupt that finds the reply queue empty; `mps_periodic` (1 s) finds faults, as on FreeBSD | core `D30`; the two M2 fault scenarios now wait for `mps_periodic` |
| D31 | IO Unit Page 7 waits 5 s with a timeout handler (FreeBSD's IDENTIFY pattern), never `mps_reinit`; the command the IOC still owns is freed late or by the next reinit; polling stops after a timeout until the next reinit; counter `IOC temperature read timeouts`, one error line. Poll on by default | core temperature `D31` |
| D29 | counter `Event acks sent`; the first ack of each event code logged at info (decides D29 on hardware) | core `D29` |
| D55 | `KASSERT hits`: every KASSERT failure and returned `panic()` counted (one counter per kext) | core `D55` |
| D46 | the family's task attribute (SIMPLE / ORDERED / HEAD / ACA) becomes the ccb's `tag_action`, as da passes its own; still no periodic ORDERED tag (comment) | glue `D46` |
| D45 | no slot table: the glue's per-task state (`struct mps_kext_task_glue`: prepared, the mapping-wait link) lives in the task's HBA data and comes back with the completion; AbortTaskRequest finds its task with the family's `FindTaskForAddress`; a counter keeps the `fMaxTasks` check. No O(outstanding) scan per IO | glue `D45` (did not build before) |
| D47 | direction IN/OUT with length 0 is a command without data (FreeBSD); an IODMACommand prepare/gen failure (no IOVA space) no longer fails the task: it is accepted, timed, and waits in order on a list (later tasks queue behind it, as behind a frozen SIM queue), mapped again after the next completions and on the 1 s timer; its timer runs from the submit (FreeBSD arms the callout before `mps_map_command`), so a timeout completes it `TaskTimeoutOccurred`; an abort, a stop or the target's removal completes it. Counter `Tasks waiting for a mapping` | glue `D47` (the zero-length part failed before; the rest did not build) |
| D7 | target ids 0 .. MaxTargets + MaxVolumes - 1 (1024 here), as FreeBSD's `sassc->maxtargets`: the core's per-id identity tables are an array allocated with `sassc->targets`; FreeBSD's `use_phynum` fallback is back in `mpssas_add_device`; the async callback passes the event's target (or all) with present/generation arrays; RETRY_UA state per id; the glue's target table has one entry per id; `ReportHighestSupportedDeviceID` = maxtargets - 1, initiator id = maxtargets; termination destroys what the family's loop misses above its limit | core `D7` (70 devices, failed at the 65th before); glue `D7`; targets (1024 ids) |
| D9 | FreeBSD's shutdown SSU: `mpssas_ir_shutdown` (its IR half removed, D53), the `enable_ssu` choice (default: SSDs only), `mpssas_SSU_to_SATA_devices`, `mpssas_stop_unit_done`, the `SSU_refcount` decrement; called by the kext's shutdown before the drain and the MUR, with RB_NOSYNC's 10 s bound (halt watchdog 30 s), the wait polls the reply queue. The INQUIRY snoop that sets `supports_SSU` uses the target's devinfo (SATA end device, not ATAPI) since our data is a physical segment list, and also runs on the fast path (SAM's INQUIRY completes without a reply frame; FreeBSD snoops only reply frames) | glue `D9` (SAS disk, SATA SSD, SATA HDD: one STOP to the SSD at shutdown, then the MUR) |
| D73 | `mps_os_dmamem_alloc` / `mps_os_dmamap_load_buf` refuse an IODMACommand that bounces (`getIOMemoryDescriptor` is the copy): `bus_dmamap_sync` is a no-op. Task buffers are safe either way (prepare/complete synchronize) | none (IOKit) |
| D74 | the tag's alignment goes to `withSpecification`; the DMA hook no longer zeroes, `bus_dmamem_alloc` does for BUS_DMA_ZERO (FreeBSD) | none (IOKit) |
| PCIe | "PCIe Link" next to "Controller Limits": negotiated speed/width (Link Status) vs the card's (Link Capabilities), MPS and MRRS in use (Device Control), MPS supported (Device Capabilities), raw registers, BAR1 mapping; at attach (one log line) and after every reinit/resume; `sas2308ctl show` prints it | `kext_pcie_test` |
| D75 | `sas2308_posted_write` (boot-arg or personality key) maps BAR1 `kIOMapPostedWrite`; after the hardware A/B, 1 in Info.plist (boot-arg `=0` reverts) | none (IOKit) |

Hardware tests and results: the deviations plan (Status, "Batch D hardware results").

Verified on the Mac (builds EBB85578 and 509E19D3, 2026-11-20): PCIe link 8.0 GT/s x4 (card x8),
MPS 128 B (card supports 4096), MRRS 512 B; "Event acks sent" 0 over 4 boots and a forced recovery
(D29 still needs hot-plug events); BAR1 posted writes +2.5 % on host-bound sequential 4K reads
(177.4k vs 173.2k IOPS), so they are the default; a forced reinit and a lost command under IO
recover with 0 errors on posted writes. Random 4K QD32 1763 IOPS, seq 1 MiB 1945.3 MB/s, verify 0
mismatches.

## Batch F: the halt's timeout cap (branch fix-batch-F, 2026-11-20)
Not a FreeBSD deviation item: a gap the 2026-11-20 03:17 "Halt/Restart Timed Out" panic
(`docs/reviews/2026-11-final.md` section 5) left after H5. Plan and rationale:
`docs/reviews/2026-11-freebsd-deviations.md`, batch F.

- **Why.** `PEHaltRestartInternal` arms a 30 s watchdog and then runs
  `IOPMrootDomain::handlePlatformHaltRestart`: first the priority power interest clients get
  `kIOMessageSystemWillRestart` / `…WillPowerOff` (SAM's block driver marks "Power Off" there),
  then the power tree is walked with `systemWillShutdown`, children (the disks) first, us after
  them. H5 fails IO that arrives after our stop, but a command already posted, or one SAM sends in
  the halt before our stop (START STOP UNIT with timeout 0, SYNCHRONIZE CACHE 30 s), keeps its
  normal deadline: D15's 10 min backstop, 30 s + 10 s for a read, 30 s reset TMs, and a reinit
  with a diag reset. A lost reply there hangs the halt until the watchdog panics.
- **Hook.** `registerPrioritySleepWakeInterest` (the KPI function SAM uses; declared locally,
  `RootDomain.h` does not compile outside xnu): our handler runs right after the watchdog is armed
  and before any `systemWillShutdown`. `systemWillShutdown` does the same if the registration
  failed. Sleep messages are ignored: sleep keeps D15's backstop (no watchdog there).
- **Cap** (`mps_xnu_halt_begin`, `mpssas_xnu_halt_caps`): every posted SCSI IO not already in
  recovery gets at most 10 s left (an untimed one too), every new one at most 10 s
  (`mpssas_action_scsiio`), every TM at most 3 s (posted ones re-armed, new ones capped at
  `callout_reset`). 10 s is FreeBSD's own shutdown figure (`mpssas_SSU_to_SATA_devices` sends each
  START STOP UNIT with a 10 s timeout and waits up to 60 s, or 10 s with RB_NOSYNC; da's shutdown
  SYNCHRONIZE CACHE has 1 h, but FreeBSD has no halt watchdog; CAM_TIME_DEFAULT 0 means "SIM
  default" and mps has none, so D15's 10 min was our own number). A spin-down that legitimately
  takes longer than 10 s is aborted; the watchdog would not have waited for it either.
- **Bounded recovery.** The normal path runs (timeout, abort, LUN reset, target reset); a reinit
  requested during the halt is H5's stop instead (`mps_xnu_halt_stop`: one MUR, 1 s ack, no diag
  reset, everything failed, new IO fails at once), and a reinit already running gives up at its
  next wait (`stop_reinit`) and stops the same way. Worst case for a lost reply: 10 + 3 x 3 + 2 =
  21 s after the halt began. Synchronous config page waits are not capped: no halt thread waits
  for them, and our stop does not wait for them beyond its 1 s drain.
- **Counters / debug.** "Halt timeouts capped", "Halt recovery stops"; `sas2308ctl halt` starts
  the halt's caps on a running system (one way: the controller stops at the next reinit; reboot
  after it).

| id | fix | host test |
|---|---|---|
| F1 | the above | glue `F` (SSU timeout 0 + read, replies lost: done 10-15 s after the halt began, no diag reset, later IO fails at once), `F-new` (IO and FORMAT UNIT after the halt: 10 s, a working abort keeps the controller), `F-tm` (no TM answered: done within 16 s), `F-pending-tm` (a LUN reset already waiting 30 s: 3 s left), `F-reinit` (a reinit stuck in its diag reset when the halt begins: stopped within 5 s); all failed before |

Verified on the Mac (build 085DA9D6, 2026-11-20): a lost read, then `sas2308ctl halt`: capped,
timed out at +10.00 s, abort / LUN / target reset, the stop instead of a reinit, the read failed
10.15 s after the halt began, 0 diag resets; restarts with a mounted JHFS+ volume clean (12 s
request to boot; 47 s with a write lost just before `shutdown -r`, recovered before the halt), no
panic, files verify; the routine launchd shutdown stall reports have no storage or kext frames.
Random 4K QD32 1772 IOPS, seq 1 MiB 1944.9 MB/s, verify 0 mismatches. Details: the deviations
plan, "Batch F hardware results".

## What is implemented (M1 – M4)
> **Names after M5.** The tables and sections below are the M1–M4 history and use the names of
> the deleted C++ core. Old → new: `MpsCore::Attach/Detach/Shutdown` → `mps_attach`/`mps_free`;
> `DiagReset/MessageUnitReset/TransitionReady` → `mps_diag_reset`/`mps_message_unit_reset`/
> `mps_transition_ready`; `RequestSync` → `mps_request_sync`; `GetIocFacts/ResizeQueues/AllocQueues`
> → `mps_get_iocfacts`/`mps_iocfacts_allocate`; `SendIocInit/InitQueues` → `mps_send_iocinit`/
> `mps_init_queues`; `RegisterEvents/HandleEvent/HandleTopologyChange` → `mps_register_events`/
> `mpssas_evt_handler`/`mpssas_fw_work`; `SendEventAck` → `mps_xnu_event_ack`; `PortEnable` →
> `mpssas_startup`; `StartIo/CompleteIo` → `mpssas_action_scsiio`/`mpssas_scsiio_complete`;
> `HandleInterrupt` → `mps_intr_msi`/`mps_intr_locked`; `TimeoutIo` → `mps_xnu_ccb_timeout` →
> `mpssas_scsiio_timeout`; `StartTm/SendTm/TmComplete/RecoveryTmDone` → `XPT_XNU_TM`,
> `mpssas_send_abort/_reset`, `mpssas_*_complete`; `Service()` → `mps_periodic` + callouts
> (`mpssas_tm_timeout`); `Reinit` → `mps_reinit` (taskqueue); `Gone()/MarkGone` →
> `MPS_FLAGS_XNU_GONE`/`mps_xnu_gone`; `FailOutstandingIo` → `mpssas_xnu_fail_all`; held IO →
> thin-CAM requeue (`cam_xnu_*`); `MpsHw::SleepMs` → `msleep`/`DELAY` (drop mps_mtx);
> `MpsStats` → `struct mps_xnu_stats`; `MpsDecode` → `mps_table.c` + `mps_xnu.c`; kext `fLock` →
> the core's `mps_mtx` (the class keeps a leaf `fLock` for its own tables); TM SMIDs 3–6 → the
> high-priority free list (SMIDs 1–4); `MaxSegs()` → `buffer_dmat` nsegments (257 at maxio 1 MiB).
> The *behaviour* described is what the port keeps; differences are listed under M5a-2/M5a-3.

| Area | FreeBSD origin | Here |
|---|---|---|
| Reset / ready | `mps_diag_reset`, `mps_message_unit_reset`, `mps_transition_ready` | `MpsCore::DiagReset/MessageUnitReset/TransitionReady` |
| Doorbell handshake | `mps_request_sync`, `mps_wait_db_ack` | `RequestSync`, `WaitDbAck/WaitDbInt` |
| IOCFacts → sizing | `mps_get_iocfacts`, `mps_iocfacts_allocate` | `GetIocFacts`, `ResizeQueues`, `AllocQueues` |
| IOCInit + queues | `mps_send_iocinit`, `mps_init_queues` | `SendIocInit`, `InitQueues` (request, reply-free, reply-post, sense) |
| Events / discovery | `mps_register_events`, `mpssas_evt_handler` | `RegisterEvents`, `HandleEvent`, `HandleTopologyChange`, `SendEventAck` |
| Port enable | `mpssas_startup` | `PortEnable` |
| SCSI IO | `mpssas_action_scsiio`, `mps_data_cb`, `mpssas_scsiio_complete` | `StartIo` (64-bit simple SGEs, chain frames below), `CompleteIo` incl. fast-path, autosense |
| Chain SGLs (M3) | `mps_add_chain`, `mps_push_sge`, `mps_alloc_chain`/`mps_free_chain`, chain sizing in `mps_iocfacts_allocate` | See "Chain SGLs" below. `MaxSegs()` = min(256, request frame + IOCFacts.MaxChainDepth chain frames, chain pool); SAS2308 (128-byte frames, depth 128): 256 |
| Interrupt | `mps_intr_msi` | `HandleInterrupt` on the work loop; MSI/MSI-X source chosen explicitly |
| Hotplug | `mpssas_add_device/remove` | `DeviceArrived/DeviceGone` → thread call → `CreateTargetForID/DestroyTargetForID` |
| Surprise removal | — | registers read 0xFFFFFFFF → `Gone()` → `FailOutstanding()` |
| Task management (M2, async M4) | `mpssas_send_abort`, `mpssas_send_reset`, `mpssas_prepare_for_tm`, `mpssas_*_complete`, `mpssas_tm_timeout` | `StartTm`/`SendTm`: ABORT_TASK, ABORT_TASK_SET, CLEAR_TASK_SET, CLEAR_ACA, LUN reset, target reset (MsgFlags LINK_RESET). Up to 4 TMs at once on SMIDs 3–6 (min(4, IOCFacts.HighPriorityCredit)), high-priority request descriptor, completed from the reply queue, timeout as a deadline in `Service()` (abort 5 s, resets 30 s, as FreeBSD). See "Asynchronous recovery" |
| Timeout escalation (M2, async M4) | `mpssas_scsiio_timeout`, `mpssas_abort_complete`, `mpssas_logical_unit_reset_complete`, `mpssas_target_reset_complete` | `TimeoutIo` + per-target `Recovery` state machine advanced from TM completions: abort each timed-out IO → LUN reset → target reset → reinit; a step succeeds only if nothing is left outstanding for the IO / LUN / target (FreeBSD rule, status ignored) |
| IOC reinit (M2, threaded M4) | `mps_reinit`, `mpssas_handle_reinit`, `mps_iocfacts_allocate(FALSE)` | `Reinit` on the kext's recovery thread, lock dropped in every wait; up to 3 attempts (nested faults); fail posted IO with `SCSI_IOC_TERMINATED` (kext → `DeliveryFailure`, retryable); held IO kept and posted afterwards; async PortEnable for rediscovery. Targets whose handle is not re-announced are destroyed (`IocReset`/`RediscoveryDone`). If every attempt fails: core goes `Gone()`, IO failed `DEVICE_NOT_THERE`, kext disables bus mastering (FreeBSD panics here) |
| Fault detection (M2) | `mps_periodic` (watchdog) | `PollFault`: doorbell FAULT or not-OPERATIONAL. Checked on every interrupt that finds the reply queue empty, inside every internal-command wait, and by `Service()` from the kext's watchdog timer (≤ 1 s); a fault makes `NeedsReinit()` true and the kext schedules the recovery thread |
| Logging (M4) | `mps_dprint` classes, `mps_table.c`, `mpssas_log_command` | `Dprint(cls, …)` → `MpsHw::LogLine` → os_log subsystem `org.sas2308.driver.SAS2308HBA` (the bundle ID), one category per class; see "Logging" |
| Multi-LUN (M2) | `MPS_SET_LUN` | `ReportHBAHighestLogicalUnitNumber` = 255. The family answers `kSCSIProtocolFeature_GetMaximumLogicalUnitNumber` with it and `IOSCSITargetDevice` does REPORT LUNS / creates the LUN nubs itself; `ProcessParallelTask` passes the family's SAM LUN bytes into the SCSI IO `LUN` field unchanged. TMs encode `SCSILogicalUnitNumber` with `MpsCore::EncodeLun` (port of `mpssas_set_lun`) |

### Chain SGLs (M3)
- Request frame 128 bytes (IOCFacts.IOCRequestFrameSize 32 dwords): the SCSI IO header is
  96 bytes, so 2 simple 64-bit SGEs (12 bytes each) fit. Chain frames are the same size
  (FreeBSD and Linux mpt2sas use the request frame size): 10 slots.
- Packing as FreeBSD `mps_push_sge`: a frame that cannot hold everything left gets
  `slots-1` simple SGEs (the last one flagged LAST_ELEMENT) then a 64-bit chain element;
  the final SGE has LAST_ELEMENT|END_OF_BUFFER|END_OF_LIST. n segments need
  `ceil((n - s0) / (c - 1))` chain frames (s0 = 2, c = 10): 3..11 → 1, 12..20 → 2, 256 → 29.
- Chain element fields as Linux mpt2sas `_base_build_sg_scmd` (FreeBSD leaves the two offsets 0
  and Length = frame size; both are known to work on SAS2308 firmware): Length = bytes used in the
  next frame, NextChainOffset = dword offset of the next frame's chain element (0 on the last),
  request `ChainOffset` = dword offset of the first chain element (0 without chains).
- Limits: `MaxSegs()` = min(`kMaxSegsCap` 256, s0−1 + depth·(c−1) + 1 with depth =
  IOCFacts.MaxChainDepth (SAS2308: 128 → 1154), what the pool holds for one IO). 256 covers a
  1 MiB transfer in 4 KiB pieces. MaxChainDepth 0 → request frame only (2).
- Pool: `IoCount() × ChainsFor(MaxSegs())` frames (64 × 29 = 1856, 232 KiB) so the family's
  `ReportMaximumTaskCount` IOs at the advertised segment count can never run it dry; allocated
  via `MpsHw::AllocDma` (64-bit IOVA, 16-byte aligned) in 32 KiB pieces (8 here) so no large
  physically contiguous block is needed. `SetChainLimit()` caps it (FreeBSD `hw.mps.max_chains`);
  `MaxSegs()` is then reduced so one IO always fits.
- Per IO: `StartIo` checks that the free count covers `ChainsFor(nsegs)` before touching
  anything; if not it returns `kMpsBusy` with nothing consumed (FreeBSD `ENOBUFS` +
  `chain_alloc_fail`; `Stats().chainAllocFail`, `chainFreeLow` = low-water mark). The IO's
  frames are a list (`ioChain_`) returned to the pool by `ReleaseIo` — the only place an IO
  slot is freed: completion (incl. fast path and TM-terminated), `FailPostedIo` (reinit, reinit
  failure, `Shutdown`) and `FailOutstandingIo` (surprise removal; refused while the IOC is
  alive). Held IO (M4) has no chain frames: they are taken when it is posted.
  Duplicate/stale completions hit the idle-SMID check and free nothing.
  `Detach` runs `Shutdown` (MUR, then every IO failed) and frees the pool. A reinit
  reallocates the pool only if the IOCFacts sizing (incl. MaxChainDepth) changed.
- Kext: `ReportHBAConstraints` reports `kIOMaximumSegmentCount{Read,Write}Key = MaxSegs()`
  (no longer hardcoded); `kIOMaximumByteCount{Read,Write}Key` = min(1 MiB, (MaxSegs()−1) ×
  PAGE_SIZE). `ProcessParallelTask` runs `gen64IOVMSegments` in a loop (16 segments per call)
  into the task's HBA data area (`ReportHBASpecificTaskDataSize` = 256 × 16 bytes, via
  `GetHBADataPointer`), not the kernel stack; `StartIo` re-checks against `MaxSegs()` under `fLock`.
  `HandleDeadLocked` (M3: `CheckController`) calls `FailOutstandingIo` when the core is gone, so SMIDs and chain frames
  are released, not just the kext slots.

SMID layout: 0 invalid, 1 internal (IOCFacts-era sync commands, PortEnable, event
registration), 2 event ack, 3…6 task management (high priority), 7…70 SCSI IO.

## Asynchronous recovery (M4)
Before M4, task management, the escalation and `Reinit` were polled with `fLock` held (and,
from `HandleTimeout`, the family work-loop gate): up to ~7 minutes in theory. M4 follows
the structure of FreeBSD `mps_sas.c`/`mps.c`; IOKit timers and a thread call replace the
callouts.

### Flow
1. **IO timeout** — the family's timer calls `HandleTimeout` (work loop). It calls
   `MpsCore::TimeoutIo(smid)` (`mpssas_scsiio_timeout`): drain the reply queue
   (`mps_intr_locked`); if the IO already completed, nothing ("almost timed out"); else
   mark it timed out (`MPS_CM_FLAGS_TIMEDOUT`). If its target already has a recovery the
   IO just waits its turn (`targ->timedout_commands`); else a `Recovery` record is
   created (`targ->tm`) and the oldest timed-out IO of the target gets an ABORT_TASK
   (`mpssas_send_abort`). Returns at once; the task completes later, when the IOC
   gives it back, with `ProtocolTimeoutOccurred`.
2. **TM completion** — the reply arrives on the reply queue (interrupt, or the TM
   waiter's pump, or `Service()`). `TmReply` records it; after the whole batch is drained,
   `TmComplete` runs the step's completion: `RecoveryTmDone`
   - ABORT_TASK (`mpssas_abort_complete`): the aborted IO came back → abort the next
     timed-out IO of the target, or finish; still outstanding → LOGICAL_UNIT_RESET on its LUN.
   - LOGICAL_UNIT_RESET (`mpssas_logical_unit_reset_complete`): nothing left on the LUN →
     continue with the next timed-out IO; else TARGET_RESET (MsgFlags LINK_RESET).
   - TARGET_RESET (`mpssas_target_reset_complete`): nothing left on the target → next/finish;
     else request a controller reinit.
   As in FreeBSD only "is anything still outstanding" counts, not the reported status.
   A TM completed without a reply frame requests a reinit (FreeBSD "NULL reply").
3. **Timers** — the kext's `IOTimerEventSource` runs `MpsCore::Service()` at most 1 s
   apart and re-arms for the earliest TM deadline: drain (a lost interrupt), `PollFault`
   (`mps_periodic`), TM deadlines (`mpssas_tm_timeout`: the SMID is marked *lost* — the
   IOC still owns it — a reinit is requested, an OS-requested TM gets
   `TmDone(kMpsTimeout)`), retry recoveries waiting for a TM SMID, the 60 s rediscovery
   warning, post held IO, flush rate-limited log summaries.
4. **Reinit** — `NeedsReinit()` (TM timeout, failed escalation, IOC FAULT) makes the kext
   schedule `fReinitCall` (thread call). `RunReinit` takes `fLock`, marks itself the
   thread allowed to drop it, calls `MpsCore::Reinit()` (`mps_reinit`): mask interrupts,
   drain, then up to 3 attempts of diag reset → READY → (first time the reset is
   actually issued: cancel TMs, cancel recoveries, `IocReset` upcall, fail posted IO
   with `SCSI_IOC_TERMINATED` = `mpssas_handle_reinit`) → IOCFacts (re-size if changed)
   → IOCInit → queues → event registration. Every wait inside goes through
   `MpsHw::SleepMs`, which drops `fLock`. Success: restore interrupt mask, async
   PortEnable (rediscovery), post held IO. All attempts failed: controller disabled
   (`Gone()`), everything failed with `DEVICE_NOT_THERE`, bus mastering cut.

### Edge cases (all exercised by the mock)
| Case | Behaviour |
|---|---|
| Other targets during a TM | IO keeps flowing; only the target in recovery has its new IO held |
| More timed-out targets than TM SMIDs (4) | extra recoveries wait (`waitTm`) and start when a TM SMID frees (FreeBSD only logs "XXX queue this target up") |
| TM times out, reply arrives later | counted (`tmLateReplies`), logged, SMID released; the reinit already requested still runs |
| Duplicate / stale completion (IO or TM) | idle SMID: ignored, counted (`staleReplies`), logged |
| Fault during the reinit (nested) | the attempt fails, the next attempt starts with a new diag reset (≤ 3) |
| Surprise removal (any time) | first all-ones read → `MarkGone`; `FailOutstandingIo` fails posted + held IO, finishes TMs (`kMpsGone`), drops recoveries; a reinit in progress stops at its next register read |
| Detach during a reinit | `StopController` sets `fStopping` + `fAbandonReinit`; the next `SleepMs` returns false, `Reinit` gives up (controller disabled); stop waits for the thread, then `Shutdown()`: MUR, everything failed `DEVICE_NOT_THERE`, TM waiters woken |
| Detach with TMs / recoveries / held IO | `Shutdown()` stops the IOC (MUR) *before* failing IO, so no buffer is returned while the IOC could still DMA into it |
| IO timeout while held | completed at once (`SCSI_TASK_TERMINATED`, timed out); never reached the IOC |

### IO arriving during recovery — IOSCSIParallelFamily semantics
`ProcessParallelTask` returns `kSCSIServiceResponse_Request_In_Process` and the core
*holds* the IO (`StartIo` → `kMpsHeld`) while its target is in TM recovery or a reinit is
pending/running; held IO is posted in arrival order when the recovery finishes / the
reinit succeeds (FreeBSD: `CAM_REQUEUE_REQ` with the device or SIM queue frozen,
`mpssas_prepare_for_tm`, `mpssas_release_simq_reinit`). IOKit has no requeue status:
returning `SERVICE_DELIVERY_OR_TARGET_FAILURE` would make IOSCSIBlockCommandsDevice
retry a few times without delay and then fail the IO, turning a recoverable reset into IO
errors; a BUSY/TASK SET FULL status is the device's to give, not the HBA's. The family's
per-task timer runs from `ProcessParallelTask`, but since batch B (D4) `HandleTimeout`
re-arms it for a task that is held or that the IOC has owned for less than its timeout
(FreeBSD's callout runs from the post), so held time never counts; a timed-out task
completes `TaskTimeoutOccurred` (D44), and an `AbortTaskRequest` for a held task completes it
aborted (FUNCTION_COMPLETE). IO failed by a reinit completes with
`DeliveryFailure` (retryable); the retry arrives as new IO and is held until the reinit is done.

### Locking rules
(M4. Since M5a-3: `fLock` → the core's `mps_mtx`; the core never sleeps with it held — every
wait goes through `msleep`/`DELAY`, which drop it; family calls are made only by
`DeliverCompletions` and the hotplug thread, with no lock held; see "Status and decisions (M5a-3)".)
1. `fLock` (IOLock = `lck_mtx`, blocking) protects `fCore`, `fSlots`, `fTargets`,
   `fTmWaiters` and the recovery flags. It is FreeBSD's `mps_mtx`.
2. No family callback (`CompleteParallelTask`, `Create/DestroyTargetForID`) with `fLock` held.
3. **No sleep with `fLock` held.** The core never waits except through `MpsHw::SleepMs`;
   the kext implements it by dropping `fLock` for the thread named in
   `fSleepUnlockThread` (the recovery thread, `InitializeController` around `Attach`,
   the stop/terminate path around `Shutdown`/`Detach`). Family TM requests wait with
   `IOLockSleepDeadline` (drops the lock) in 10 ms steps and pump the reply queue
   themselves. `QuiesceHotplug`/`StopReinit` wait with `IOLockSleep`.
4. While the recovery thread sleeps, every other entry point can run and sees
   `InReset()`: `StartIo` holds, `HandleInterrupt`/`Service` return at once, `StartTm`
   is busy, `TimeoutIo` only marks the IO (the reinit fails it), `HandleDeadLocked` waits
   for the thread. `fStopping` blocks new IO/TM/reinit during stop.
5. **Worst-case `fLock` hold now**: one core call between two waits — draining the reply
   queue (≤ post-queue depth descriptors, ~100), failing ≤ 64 IOs, a register handshake
   step, or, during a reinit whose IOCFacts changed, freeing and reallocating the DMA
   queues. Microseconds normally, a few milliseconds at most (versus ~7 min in M3). The
   longest waits are now outside the lock and not on the work loop: a family TM request
   blocks *its caller* up to its TM timeout (5 s abort, 30 s resets) + 10 ms; a reinit
   runs on its own thread for as long as the firmware needs (diag reset ≤ 300 s,
   READY ≤ 60 s); `StopController` waits for the recovery thread to notice the
   abandon flag (≤ one sleep, ≤ 256 ms) and for the MUR handshake (≤ 5 s, lock dropped).

### Deliberate deviations from FreeBSD
(M4 list; superseded by the M5 list under "Status and decisions (M5a-3)": callouts and
CAM requeue are FreeBSD's own again, TM SMIDs come from the high-priority free list.)
- Callouts → a deadline per TM checked by `Service()` from one `IOTimerEventSource`.
- `mps_reinit` runs on a thread call instead of in callout context; it retries the
  reset sequence on a nested fault (FreeBSD panics) and disables the controller after 3
  failed attempts (FreeBSD panics).
- New IO during recovery is held in the core (no CAM requeue in IOKit).
- TM SMIDs are a fixed range of 4 (FreeBSD: high-priority free list sized by
  HighPriorityCredit); a recovery waits for a free one instead of being dropped.
- TM replies are evaluated after the reply batch, not inline.
- Posted IO is failed as soon as RESET_ADAPTER has been written (the IOC no longer owns
  it), with `IOC_TERMINATED` even if the controller is then found dead.

## Logging (M4)
Port of FreeBSD's `mps_dprint(sc, class, ...)`: every line carries a set of debug
classes (`mpsvar.h`); a line is emitted if any of its classes is in the mask
(`hw.mps.N.debug_level`). Values are decoded with the `mps_table.c` tables (event,
IOCStatus, SCSI status, TM response, link rate, PHY status, WhoInit, discovery reason,
device-status reason, `%b` bit fields). Sense keys / ASC-ASCQ / CDB names come from small
SPC tables (FreeBSD uses CAM's `scsi_sense_desc`/`scsi_op_desc`).

| Class (category) | What |
|---|---|
| `fault` | IOC FAULT with code, reset/handshake failures, failed attach / resume, controller disabled (a surprise removal is `error`: an unplugged dock is a user action) |
| `error` | failed IO (decoded: IOC/transport failures, CHECK CONDITION MEDIUM/HARDWARE ERROR), stale/duplicate/malformed replies, dropped event acks, DMA failures |
| `recovery` | IO timeouts, every TM sent/completed (type, SMID, target, IOCStatus, response, terminations, loginfo, ms), escalation steps, TM timeouts and late replies, held IO posted, reinit requests |
| `info` | the important subset of all classes (FreeBSD default is `info,fault`) |
| `init` | attach, IOC state transitions (doorbell, WhoInit, fault code), IOCFacts / IOCInit summary, queue sizes, reinit phases with durations, PortEnable, detach |
| `event` / `mapping` | every event (name, context, ack) and its field dump (`mps_print_evt_sas` via `MPS_DPRINT_EVENT`, gated on `event`, not `xinfo`), topology PHY entries (handle, PHY status, link rate), device arrive/gone with target ID, target create/destroy |
| `xinfo` | `mps_print_iocfacts` / `mps_print_portfacts` field dumps, each DMA allocation (size, alignment, IOVA), held IO, Freezing/Unfreezing devq, IOs completed with a SCSI status (see below) |
| `trace` | one line per IO start (SMID, handle, LUN, CDB name, LBA, blocks, length, segments, chains) and completion (status, transfer, latency) |
| `user` | task management requested by the OS |

Levels (the user-facing knob) are presets of that mask:
0 error = fault,error · 1 warn = fault,error,recovery · **2 info (default)** = fault,error,info ·
3 debug = everything but trace · 4 trace = everything. The default is FreeBSD's (`mps.c`:
`sc->mps_debug = MPS_INFO|MPS_FAULT`) plus `error`, since no CAM periph prints IO errors on macOS
(D22). FreeBSD tags the recovery lines that matter (command timeout, abort / reset sent and
finished, reinitializing) `MPS_INFO|MPS_RECOVERY`, so they show at info; `recovery`, `init`,
`event` and `mapping` alone are opt-in (debug, or `sas2308ctl mask +recovery`). warn is the
errors plus the whole recovery trail without the info lines (not a subset of info). A raw mask
(`SAS2308DebugMask`, number or `mps_parse_debug` list such as `info,fault,+recovery`) overrides
the level. Changing the level or mask and the debug triggers log one `info` line whatever the mask.
Before D22 the default also had recovery/init/event/mapping: every event dumped field by field, a
`Doorbell=` line per 50 ms poll of `mps_transition_ready` and both devq lines of every TM persisted.

**Where lines go.** os_log subsystem `org.sas2308.driver.SAS2308HBA` (the bundle ID), message prefixed
`hbaN:`. One class of the line decides both the category and the type (D24), picked in the order
xinfo, trace (most verbose: a line tagged `xinfo` is detail whatever else it carries), fault,
error (most serious), recovery, event, mapping, init, log, user, info: `MPS_RECOVERY|MPS_INFO` is
`recovery`/DEFAULT, `MPS_ERROR|MPS_RECOVERY` is `error`/ERROR, `MPS_XINFO|MPS_RECOVERY` is
`xinfo`/DEBUG. os_log type of that class: fault → FAULT; error → ERROR;
info/recovery/init/event/mapping → DEFAULT (shown by `log stream`, persisted);
xinfo/trace/log/user → DEBUG (`--level debug`). Boot-arg `sas2308_iolog=1` (or
property `SAS2308LogToIOLog`) sends them to IOLog instead (`dmesg`, survives into panic logs);
lines without a device (printf, hexdump, KASSERT, thin CAM warnings) follow the last attached
controller's switch and carry its unit (D25).
The kext adds no line the core already prints (D23): an IO `mpssas_action_scsiio` refused is
the core's line (NULL handle, shutting down, cannot map) or none (target being removed), as on
FreeBSD; the kext logs only refusals that never reached the core (invalid request: error;
detached or another disk behind the id: xinfo); no line for AC_BUS_RESET / AC_SENT_BDR; one
`recovery` line per OS task management request with its outcome; a timeout racing its
completion is xinfo.
Format strings must not use `%{public}`/`%{private}`: the kernel os_log encoder rejects any
`%{` (EINVAL, line dropped, msgbuf shows "{public}s"); kernel string arguments are always public.
`make check` greps for it.

**Stream / read.**
```
/usr/bin/log stream --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'                     # info level
/usr/bin/log stream --level debug --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'       # debug/trace lines too
/usr/bin/log stream --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA" AND category == "recovery"'
/usr/bin/log show --last 1h --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'             # history
sudo log config --subsystem org.sas2308.driver.SAS2308HBA --mode "level:debug,persist:debug"   # keep debug lines
```

**Change the level.**
- Default: `SAS2308LogLevel` in the Info.plist personality (2).
- Boot: `sudo nvram boot-args="… sas2308_log=3"` (0–4) and/or `sas2308_debug=0x7ff`
  (raw mask), reboot.
- Runtime (until unload): `make -C tools`, then `sudo tools/sas2308ctl level debug`,
  `sudo tools/sas2308ctl mask +trace`, `sudo tools/sas2308ctl iolog on`. Uses
  `IORegistryEntrySetCFProperty` → `setProperties` (root only). Current values:
  `tools/sas2308ctl show` or `ioreg -r -c SAS2308HBA -k SAS2308DebugMask`.

**Counters.** `ioreg -r -c SAS2308HBA -k SAS2308Statistics` or `tools/sas2308ctl stats`:
IOs completed, read/write bytes, latency avg/max (µs, post → completion incl. hold time),
tasks accepted/held/rejected, errors by class (check condition, SCSI status, terminated,
device gone, transport), IO and family timeouts, TMs by type, TM timeouts / late replies /
failed, recoveries and the step that cleared them, IOC faults, diag resets, reinits
(failed attempts, max ms), stale/bad replies, events, dropped acks, chain-pool failures and
low-water mark, debug mask, state. (Since M5: `sc->xnu.stats` under mps_mtx; the kext's
1 s timer publishes them; the rate-limit summary is flushed by `mps_periodic`.) Kept as plain increments under `fLock`; published by
the watchdog at most once a second and only when something changed; also logged at stop.
`sudo tools/sas2308ctl reset-stats` zeroes them.

**Which failed IO is an error (branch log-noise, 2026-11-20).** As FreeBSD. A command the
target completed with a SCSI status (CAM status `SCSI_STATUS_ERROR`: IOCStatus success, data
underrun or recovered error with status check condition, busy, task set full, …) is printed by
`mps` only at `MPS_XINFO` (`mpssas_scsiio_complete`: `mpssas_log_command(cm, MPS_XINFO, "ioc %x
scsi %x state %x xfer %u")`, `mps_sc_failed_io_info`); CAM decides what the status means. So the
line is `xinfo` (os_log DEBUG, not persisted). Exception, kept `error`: CHECK CONDITION with sense
key MEDIUM ERROR (0x3) or HARDWARE ERROR (0x4) — FreeBSD's CAM prints that sense on the console
(`scsi_all.c` `sense_key_table`: `SS_RDEF` includes `SSQ_PRINT_SENSE`, `cam_periph_error` →
`cam_error_print`) and IOSCSIArchitectureModelFamily logs no sense, so our line is the only trace.
NOT READY 0x04/0x02 is `SS_START` in CAM (silent). ABORTED COMMAND (0xB, also `SS_RDEF`) stays
`xinfo`: SAT maps an ATA ABRT, e.g. an unsupported passed-through command, to it. Every other
CAM status (IOC/transport errors, autosense failed, device gone, timeouts, aborts, resets) is
unchanged. Counters are unchanged (every check condition is "Err check condition"). Why: spun-down
SATA drives answer the family's TEST UNIT READY polls (~3 s per drive) with NOT READY 0x04/0x02
("initializing command required"); at `error` that was ~140 persisted lines a minute (~7,000 per
boot), burying real errors and overwriting the kernel msgbuf within minutes. The class → os_log
type map is `mps_xnu_log_type()` (core, host-tested; the kext's `LogType` calls it).

The line ends `len N IOC TransferCount M`: N is the request's DataLength (`dxfer_len`), M the
reply frame's field, used (as in FreeBSD) only for DATA_UNDERRUN. A no-data CDB ends `no data`:
it is sent with DataLength 0 and `NODATATRANSFER` (the glue forces length 0 for direction none;
trace on the Mac: `IO start … TEST UNIT READY … len 0 segs 0`), and its reply's TransferCount is
not meaningful — the SAS2308 left 0, 512, 1536, 4096 … 65536 in it for SATA TEST UNIT READY
answered NOT READY (30 min sample: 3977 × 65536, 108 × 0, 83 × 512, …), which the old line
printed as "xfer 65536/0". Cosmetic: the family is told 0 bytes (`dxfer_len - resid`).

**Rate limiting.** Failed-IO lines (not terminations from our own recovery) are limited
per target (the limiter is a member of `struct mpssas_target`, D26; it was 8 slots hashed on
handle mod 8, shared by targets and naming the last one) to 5 per 10 s (error lines only;
`xinfo` lines are debug-level, opt-in, not limited); then one line "target N (handle H): M
similar IO error lines suppressed", printed with the next line after the window or by
`mps_periodic` within a second of the window closing.

**Hot-path cost below debug.** A disabled line costs one relaxed load and an AND (the
`mps_dprint` macro skips argument evaluation); per IO there are always two
`mach_absolute_time` reads and a few counter increments; no allocation, no registry update.

## Stubbed / deferred (after M5)
- Event acks: an ack that finds no free command is dropped and counted (`event_ack_dropped`);
  no queueing.
- Persistent mapping (`mps_mapping.c`) not ported: target IDs are handed out by `mps_xnu_get_tid`
  (the last handle of an ID is preferred after a reinit). SAS Device Page 0 is read since M5b
  (SAS address, device info); no SATA identify. Device removal is FreeBSD's (target reset, then
  SAS_IO_UNIT_CONTROL REMOVE_DEVICE once the target's commands are gone).
- os_log handles (one per category) and one `IOLock` (the `IOLockWakeup` argument) are created
  once per kext load and never released.
- (Done in M5b: the FreeBSD-named compat symbols are no longer exported.)
- Not verified against Apple's binaries: whether the family/upper layers retry
  `DeliveryFailure` / `ProtocolTimeoutOccurred` completions, whether `AbortTaskRequest` expects
  the aborted task to be completed by the HBA (we complete it), that `HandleTimeout` may return
  before the task is completed (completed later, when the IOC gives it back — FreeBSD and Linux
  do the same), and whether the family calls the `*Request` TM methods with its work-loop gate
  held (the waiter pumps the reply queue itself, so it works either way). (The family calls
  `ReportHBAConstraints` after `InitializeController`: IOSCSIParallelFamily source.)
- `mps_pci_restore` only turns memory decode and bus mastering back on after a diag reset
  (FreeBSD restores the saved config space); whether the SAS2308 diag reset clears more of its
  config space is to be checked on hardware.
- The kext glue (thread calls, IOKit DMA, interrupts, the family interface) is compiled and
  link-checked only; the core it drives runs under the mock.
- No RAID/IR, no SMP passthrough, no firmware flashing, no target mode.
- Power management (sleep/wake) and the shutdown stop are implemented (M5b) but untested on
  hardware; until the hardware plan's sleep steps pass, don't sleep the Mac with disks mounted.
- The HBA's own SAS address is not published (needs SAS IO Unit Page 0; the family's
  `SetHBAProperty` has no SAS key). `IOPCIPauseCompatible` not set (would need a paused state).

## Risks
1. **Interrupts.** No INTx through a TB tunnel on Apple silicon; the kext picks the
   `kIOInterruptTypePCIMessaged` source. If IOPCIFamily exposes none, it falls back to
   index 0 and logs — then no completions arrive (commands time out). First thing to check on hardware.
2. **DMA / DART.** All buffers go through `IOMapper::copyMapperForDevice` + `IODMACommand`
   (`kMapped`), so the card sees IOVA, not physical addresses. With a mapper the core's
   buffers are not physically contiguous (the DART gives one IOVA range; a tag with one
   segment fails the allocation otherwise); IOVA < 4 GiB for the tags with a 32-bit lowaddr.
3. **Panics.** It is kernel code: a bad pointer or a stuck lock panics the whole Mac.
   Test with nothing important running; first loads with no disks attached.
4. **TB hot-unplug.** Unplugging the dock with disks mounted = surprise removal path
   (outstanding IO failed). Tested only in the mock.
5. **Firmware variance.** The mock reports fw 20.0.0.0 / MPI 2.0. Real 9207 IT firmware
   P20 should match; IR firmware or very old firmware is untested.

## Verified (2026-11-19, after M5a-3)
- Kext builds on the Mac (CLT SDK 27.2, arm64e, `make` rc 0): no compiler warnings; the only
  two lines are the linker's "libkmod.a built for 27.2, linking 27.0" notes (`MINOS` 27.0,
  unchanged since M1). Ad-hoc signature verifies.
- `kmutil libraries -p build/SAS2308HBA.kext --undef-symbols` (rc 0): all 387 undefined symbols
  resolve (= `nm -u`), all public KPIs: kpi.iokit 244, kpi.libkern 83, IOSCSIParallelFamily 44,
  kpi.mach 13, IOPCIFamily 3; nothing from kpi.private/unsupported (M4: 382 — kpi.iokit 243,
  kpi.libkern 81, IOSCSIParallelFamily 44, kpi.mach 11, IOPCIFamily 3). New in kpi.mach:
  `thread_call_enter_delayed`, `nanoseconds_to_absolutetime` (callouts).
- Mock tests on the Mac (Apple clang 21, ASan/UBSan, `make -C test CXX=clang++ CC=clang`):
  `mock_ioc_test` PASS (8070 SGLs walked, 122086 log lines), `mps_port_test` PASS (141 checks), no
  sanitizer reports, no build warnings. Same on Linux (g++ 13.3, ASan/UBSan/LSan);
  `make -C test freestanding` clean.
- `clang --analyze` (Apple clang 21; default checkers + optin.*, security.ArrayBound,
  alpha.core.CastSize/PointerArithm, alpha.unix.cstring.*) on `core/*.c` and `core/compat/*.c`:
  fixed two findings — a CDB copy bounded only by `cdb_len` (`mpssas_action_scsiio`; FreeBSD's
  CAM bounds it, the thin CAM did not; a mock check added) and a NULL check on `tm->cm_targ` in
  `mpssas_xnu_tm_complete` that made the analyzer assume NULL for `mpssas_free_tm` (it is always
  set). Left as is, all in FreeBSD's code unchanged: `optin.performance.Padding` on `struct
  mps_softc` (FreeBSD's layout), a dead store in `mps_transition_operational`, and a NULL `rep`
  path in `mpssas_scsiio_complete` (false positive: `rep` is `cm->cm_reply`, checked just before;
  the analyzer assumes the logging call changes it).
- `tools/sas2308ctl` builds (no warnings) and reports "no SAS2308HBA instance" (kext not loaded).
- NOT verified: anything at runtime in the kernel.

## Verified (2026-11-19, after M4)
- M4 kext builds on the Mac (CLT SDK 27.2, arm64e, no new compiler warnings);
  `kmutil libraries -p build/SAS2308HBA.kext --undef-symbols`: all 382 undefined symbols
  resolve, all in public KPIs (kpi.iokit 243, kpi.libkern 81, IOSCSIParallelFamily 44,
  kpi.mach 11, IOPCIFamily 3) = `nm -u` count. New: `os_log_create`, `_os_log_internal`,
  `PE_parse_boot_argn` (kpi.iokit), `IOLockSleepDeadline`, `IOUserClient::clientHasPrivilege`,
  `current_task`, `absolutetime_to_nanoseconds`.
- Mock (38 scenarios, 469 CHECKs, 8066 SGLs walked, 18292 log lines formatted with every
  class enabled): PASS on Linux g++ 13.3 with ASan/UBSan/LeakSanitizer and on the Mac with
  Apple clang 21 ASan/UBSan; no sanitizer reports. New M4 scenarios: abort on one target
  while the other target's IO completes (no `SleepMs` during TimeoutIo/TM handling, new
  IO to the target held then posted); abort → LUN reset → target reset → reinit without a
  single sleep, then a reinit during which "other threads" (the mock's sleep hook) submit
  IO (held), take interrupts (ignored), send TMs (busy), time out a held IO (completed)
  and call `Reinit` again ("Reset already in progress"); TM timeout at 5.1 s (timer
  re-armed for the deadline), late reply, duplicate TM and IO replies; OS TMs timing out
  and being cancelled by a reinit (`kMpsAborted`); 6 targets timing out with 4 TM SMIDs;
  nested fault during reinit (event registration faults, attempt 2 succeeds); surprise
  removal mid-recovery (posted + held IO, recovery TM and OS TM all finished once);
  surprise removal while the reinit thread sleeps; detach abandoning a sleeping reinit;
  `Shutdown` with TMs, recovery and held IO (MUR before failing). Logging tests: level
  presets, `mps_parse_debug` forms, every `mps_table.c` table/`%b`/devinfo/sense/loginfo
  decoder, attach/IOCFacts/IOCInit/queue/DMA/event/topology lines, decoded failed-IO line,
  rate limit (5 of 20 lines + "15 similar … suppressed" after the window), counters
  (IOs, bytes, latency), no lines for good IO at info, exactly 2 per IO at trace, silent
  but working at mask 0. Every IO completes exactly once in every scenario.
- Static analysis of `core/` (`MpsCore.cpp`, `MpsDecode.cpp`): `clang++ --analyze`
  (Apple clang 21, default + optin.*, security.ArrayBound, alpha.core.CastSize/PointerArithm,
  alpha.unix.cstring.*) and `g++ -fanalyzer` (13.3): clean (M4 fixed one finding: the
  SGL pointer is now derived from the frame base, not the one-element `SGL` member).
- `tools/sas2308ctl` builds on the Mac and reports "no SAS2308HBA instance" (kext not loaded).
- NOT verified: anything at runtime in the kernel (threads, timers, os_log, setProperties).

## Verified earlier (2026-11-18, after M3)
- Builds on the Mac (macOS 27.0, CLT SDK 27.2, arm64e); ad-hoc signed.
- `kmutil libraries -p build/SAS2308HBA.kext --undef-symbols`: all 361 undefined
  symbols resolve (kpi.iokit 242, kpi.libkern 67, IOSCSIParallelFamily 44, kpi.mach 6,
  IOPCIFamily 2) — matches `nm -u` (361). M3 added `GetHBADataPointer`/`GetHBADataSize`.
- M3 mock (25 scenarios, 247 CHECKs, 8065 SGLs walked; PASS on Linux g++ 13 with
  ASan/UBSan/LeakSanitizer and on the Mac with Apple clang 21 ASan/UBSan). The IOC model
  now walks chain SGLs like firmware: element types, 64-bit flag, direction flag,
  LAST_ELEMENT before each chain, END_OF_BUFFER/END_OF_LIST only on the last SGE,
  `ChainOffset`/`NextChainOffset` positions, exact chain `Length`, 16-byte chain alignment,
  depth ≤ MaxChainDepth, every address inside a mapped buffer; it moves the data (reads get a
  position-dependent pattern, writes are captured and compared), and re-walks a held IO's SGL
  when it finally completes/terminates it (chain frames must still be intact). Tests: 1, 2,
  3, 10, 11, 12, 17, 20, 21, 255, 256 segments read+write with scattered, shuffled segments;
  1 MiB as 256 × 4 KiB and 3 × 16 MiB−1 (max SGE length), 16 MiB SGE rejected; MaxSegs+1
  rejected; IOCFacts variants (depth 0/1/3, 256-byte frames, too-small frames); pool
  exhaustion with `SetChainLimit(5)` (busy, nothing consumed, smaller IOs still go, recovers
  after completions and across a reinit); chains released after ABORT_TASK, LUN reset,
  reinit with three chained IOs, duplicate completion, surprise removal
  (`FailOutstandingIo`); 3000-IO fixed-seed fuzz (1–256 segments, 1 B–12 KiB, up to 8 in
  flight, pool back to full after each batch). Mutation check: dropping LAST_ELEMENT,
  NextChainOffset, ChainOffset, exact Length, or the chain release each fails the test.
- M2: mock IOC test PASS (19 scenarios, 116 CHECKs) under ASan/UBSan + LeakSanitizer with g++ 13
  (Linux) and under ASan/UBSan with Apple clang 21 (Mac). M1: attach, port enable + discovery
  + ack, 2-segment read with fast-path completion, CHECK CONDITION with autosense, argument
  checks, 5000 mixed IOs with queue wrap, surprise removal, detach. M2: IO to LUN 3 (LUN
  bytes in the request), abort of an outstanding IO (high-priority descriptor, TaskMID,
  TASK_TERMINATED), LUN reset (only that LUN), target reset (LINK_RESET), clear task set,
  escalation abort→LUN→target reset, escalation ending in reinit (bystander IO failed
  IOC_TERMINATED, rediscovery, interrupts restored), TM timeout → straight to reinit, lost
  completion → reinit → IO works again, IOC FAULT mid-IO detected on an empty interrupt →
  reinit → IO works, FAULT during a TM → reinit, 9 kinds of garbage reply descriptor ignored,
  reinit failure (diag reset stuck) → `Gone()` with DEVICE_NOT_THERE, no DMA leaks.
- Static analysis of `core/` (re-run after M3, clean): `clang++ --analyze` (Apple clang 21; default checkers plus
  optin.*, security.ArrayBound, alpha.core.CastSize/PointerArithm, alpha.unix.cstring.*) and
  `g++ -fanalyzer` report nothing.
- NOT verified: loading, matching, interrupts, real DMA — needs the card and the security change.

## Hardware test plan (when the card + dock arrive)
0. Optional sanity: put the 9207 in a Linux PC first; Linux `mpt3sas` dmesg shows firmware
   version and `Protocol=(Initiator[,Target])` — confirms the card and its firmware.
1. Dock + card on the Mac, kext NOT loaded: `ioreg -r -c IOPCIDevice | grep -A3 0087` → device enumerates through the tunnel.
2. Load kext with **no disks attached**:
   `log stream --level debug --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'`
   → expect "log level 2 (info)", IOC state lines, IOCFacts / IOCInit / queues lines,
   "using message-signalled interrupt index N", the core's port-enable / discovery lines
   (FreeBSD wording since M5); `tools/sas2308ctl stats`.
3. Attach one scratch disk: discovery event → target created → disk appears in `diskutil list`.
4. Read-only test: `dd if=/dev/rdiskN of=/dev/null bs=1m count=4096` → throughput, no timeouts.
   Check the attach log line "max N segments per IO" (expect 257 since M5: maxio 1 MiB + 1)
   and the core's IOCFacts / chain sizing lines against the real IOCFacts.
5. Write test on the scratch disk only, then fsck/compare.
6. Unload/reload, dock unplug with idle disk.
7. Recovery on real firmware: pull a disk mid-`dd` (timeout → abort/reset path), and
   `sg_reset`-style LUN/target resets if a userland path to them exists. Watch the
   `recovery` category; meanwhile IO to a second disk must keep going, and
   `tools/sas2308ctl stats` must show the TM / recovery / reinit counters move.
8. Properties (M5b): `ioreg -l -w0 | grep -E "Physical Interconnect|SAS Address"` → the HBA and
   every target nub show SAS / External, every target an 8-byte `SAS Address` matching the disk's
   label/`sg_inq` (big-endian byte order, nub named `SAS Target 5000…`); `ioreg -r -c SAS2308HBA -k
   "Controller Characteristics"` → LSI, SAS9207-8i, the firmware version (compare with the IOCFacts
   init line), 6 Gigabit; System Information → the disk shows Physical Interconnect SAS, Location
   External (screenshot). Check `IOPCITunnelled` in the attach log line.
9. Sleep / wake (M5b) — first idle, then with a scratch disk mounted:
   `log stream --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'` in a second session,
   `pmset sleepnow`, wake after 1 min. Expect "power state 1 -> 0", "system sleep: quiescing",
   "suspended: IOC stopped", then "power state 0 -> 1", "resuming", a reinit and rediscovery with
   the same target IDs; `diskutil list` unchanged; `SAS2308Statistics` Suspends = Resumes = 1. Then
   sleep in the middle of a `dd` read: no IO errors in the `dd`, "Suspend drain left" 0 (or IO
   retried after wake). Also: dock unplugged while asleep → after wake the targets are destroyed,
   no panic. Watch for "accessed while suspended" (must never appear).
10. Restart with the dock powered (M5b): `sudo shutdown -r now` with the kext loaded and a disk
   attached; the log (`log show --last boot-1`… or `sas2308_iolog=1` + panic-free boot) should show
   "restart: quiescing" / "controller stopped (message unit reset)"; after the reboot the kext
   attaches normally (IOC found READY, not OPERATIONAL/faulted) and the disk is rediscovered.
   Same with `shutdown -h` and the dock left on.
11. Termination (M5b): `kmutil unload` with targets present → every nub gone
   (`ioreg -r -c IOSCSIParallelInterfaceDevice` empty, including ID 63 if used); dock unplug during
   discovery (hotplug thread inside CreateTargetForID) → no panic, no leftover nub.

## Updating from FreeBSD

`core/` is a vendor import: the branch `vendor/freebsd-mps` holds the pristine upstream files at
our paths, master descends from it, and `git diff vendor/freebsd-mps master -- core/` is the whole
port delta. `core/VENDOR.txt` is the only record of the upstream commit and has the path map, the
files not ported, the files derived from FreeBSD without being vendored, which upstream line to
follow (FreeBSD 14.x release tags), and the update procedure: `tools/import-freebsd.sh
release/X.Y.0` commits the new pristine files on the vendor branch and tags them, then a branch
from master merges `vendor/freebsd-mps` and resolves conflicts by the rules there (#include
blocks: take ours and check for a newly needed include; `/* XNU: */` code: keep the deviation,
carry the fix). Tried with real upstream commits on 2026-11-20: stable/14 merged cleanly, main
gave 12 conflict hunks in 6 files, half of them #include blocks. The review of this setup is
`docs/reviews/2026-11-vendor.md`.
