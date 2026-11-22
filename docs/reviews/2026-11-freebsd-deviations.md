# SAS2308 kext vs FreeBSD mps / CAM: deviations to fix

Reviewed: sas2308-dext **master 4596c06** against FreeBSD 8c9ce319fef7 (`sys/dev/mps`, `sys/cam`) and Apple's
IOSCSIParallelFamily / SAM / IOPCIFamily sources. Full reasoning, every question and its answer:
`decisions-qa.md` (81 entries, D1-D81, all RESOLVED after 4 rounds). Line numbers: ours on master 4596c06,
FreeBSD in that tree. Out of scope: the check-condition log level and the TUR "xfer 65536/0" question (branch
log-noise); items FINAL.md already fixed.

## Summary

- **81 decisions reviewed.** 46 change code (8 of them also need a hardware test), 2 are hardware tests only,
  33 stay as they are (some with a corrected or new comment: last table).
- **4 likely bugs**, all checked against the code:
  1. **D1**: `MPSSAS_TARGET_XNU_REMOVE_WAIT` and `MPS_TARGET_IS_SATA_SSD` are both `1 << 6`. The first freed
     task-management command sends any SATA SSD a target reset and REMOVE_DEVICE.
  2. **D2**: a timed-out SATA IDENTIFY keeps the single config page buffer for good (FreeBSD's target-reset
     kick was deleted). Every later page-0 read, hot-add and temperature read fails until a reinit.
  3. **D3**: the UNIT ATTENTION a drive returns after our reset reaches SAM unchanged. SAM sends SYNCHRONIZE
     CACHE once and fails on non-GOOD, so an fsync barrier after a reinit / wake / target reset can fail.
     (Hardware confirmation still needed.)
  4. **D5**: on a Thunderbolt surprise removal, `willTerminate` lets the family destroy the nubs while their IO
     is still in the core; the completions arrive on destroyed nubs.
- **Theme.** Most deviations were choices, not macOS requirements: a compat limitation pushed into the core
  (single config buffer, QFRZN release, DELAY dropping the lock), new code where FreeBSD's would do (OS TM path,
  temperature failure mode, resume reset), or Linux/M4 habits (log defaults, extra lines). The fixes move the
  difference into compat/glue and put FreeBSD's text back in the core.
- **Not a deviation worth fixing**: queue sizing, reply hardening, no-panic policy, thread-call callouts, DMA
  through the DART, interrupts from the family, MSI only, no persistent mapping (batch 2 of decisions-qa.md).

## Fix batches (one kext install each, in this order)

| Batch | Contents | Host tests (fail before / pass after) | Hardware test after the install |
|---|---|---|---|
| **A. Likely bugs and data safety** | D1, D2, D5, D3, D8, D14, D20 | SSD in the mock + freed TM; stuck PASSTHROUGH -> target reset -> next add works; all-ones + willTerminate -> no completion after destroy; 29/00 after reset -> SYNCHRONIZE CACHE GOOD; reinit with a timed-out IDENTIFY -> same tid/gen, IO held then released; swap -> no IO to the new disk under the old nub; removal TM answering after 25 s -> no early completion; autosense-fail / reset-killed R/W / DEV_NOT_THERE mappings | F_FULLFSYNC after a forced target reset and after sleep/wake under IO; 5x sleep/wake with a mounted JHFS+ volume under writes (no eject, 0 generation changes); pull one disk and replug under IO; dock unplug under IO |
| **B. Timeouts, recovery, resume, stop** | D4, D15, D44, D10, D11, D12, D13, D19, D18, D51, D43 | hold 25 s then post -> no TM; SSU with timeout 0 and no reply -> recovery after 10 min (virtual); OS abort during a recovery TM -> one TM per target; one all-ones read -> no disable; diag reset fails then succeeds -> restore called twice; nsegs over chain capacity -> EFBIG, guard pattern intact; reply posted without interrupt + unload -> CAM_REQ_CMP | reply-drop injection (timeout -> abort -> completes); 5x sleep/wake logging the resume path (READY path vs diag reset) and UA count; MSI address/data before/after a forced diag reset; unload with outstanding IO |
| **C. Compat fidelity (core back to FreeBSD text)** | D6, D16, D21, D32, D33, D35, D37, D39, D40, D41, D42, D54, D27, D36, D17 (cosmetic), D67 (assert), D78 (QUEUE_MACRO_DEBUG in host build) | two concurrent page reads + stuck IDENTIFY -> both succeed; 1000 page reads -> live mappings back to 0; DELAY keeps mps_mtx; held-queue run bounded with 2000 posted + 500 held; tunables read from personality/boot-args; boundary split in load_ccb; queue.h debug macros on | replug a breakout under IO: all drives back, "Config page failures" 0 (only then remove the M5c retries/re-add slots, as a second step in this batch or the next); random 4K QD32 unchanged |
| **D. Kext glue and IO path** | D7, D9, D30, D31, D45, D46, D47, D48, D52, D29 (ack counter), D55 (KASSERT counter), D73/D74 (asserts, minor) | 70-device topology -> 70 nubs; SSU sent to SATA SSDs only at shutdown; temperature poll timeout -> no reinit, polling stops; zero-length IN/OUT accepted; IODMACommand shortage -> requeue; task attribute passed | random 4K QD32 before/after (D45 slot removal); temperature property updates; "Event acks sent" after boot + hot-plug (decides D29); D9 needs a SATA SSD (SMART unsafe-shutdown count over 3 `shutdown -r`) |
| **E. Logging (after log-noise is merged)** | D22, D23, D24, D25, D26, D49(b) | captured log types: default mask info,fault,error; XINFO lines DEBUG; FAULT type only for IOC fault / disabled; iolog fallback for device-less lines; per-target limiter | 5 min idle + hot-plug: persisted line counts by type |
| **F. Shutdown timeout cap (new, after E)** | F1 | SSU (timeout 0) and read with lost replies, halt begins -> both done 10-15 s later, no diag reset; IO after the halt -> 10 s; no TM answered -> done < 16 s; a 30 s LUN reset pending -> 3 s; a reinit stuck in its diag reset -> stopped < 5 s | `sas2308ctl lose 1` + `sas2308ctl halt` (deterministic); `lose 1` right before `shutdown -r now` with a mounted volume; a normal mounted restart; regression |
| **G1. Code fixes and reverts to FreeBSD (after F)** | D29, D22, D15a, D55b, D58, L12, L13, L4, L11, L3, L17, L7, L10, D43 | per item in design.md "Batch G1" (each failed before: ack asserts, default mask, plist value, one diag reset, max tasks, ASan bad-free, NULL target array, unreleased freeze, no KASSERT, QMD fault, HIS ignored, lock gone after Fini, nested hotplug, pump while not in the gate) | standard regression; `lose 1` + `reinit` under IO on all 7; sleep/wake under IO; `shutdown -r now` with a mounted JHFS+ volume; default log at idle and in the recovery |
| **G2. FreeBSD defaults decided by experiments (after G1)** | D46, D21, D50, D15b, D65, core-callout IO timer, wake log class | per item in design.md "Batch G2" (D46 ORDERED cadence, D21 hot-add under a full pool, D15 SSU 60 s, D50 attach from OPERATIONAL, C1/D4 without the family timer, D43 kick, G2-wake) | D65 QD1 next to QD1024/2000 and D15b spin-up (before the install); D46 A/B (H soak, random 4K QD32), IO timer A/B (F1 4K point), D50 restart without the MUR (attach state, DART), restarts with spun-down drives; regression |
| **S1. No idle spin-down (feature request, after the 14.5.0 import)** | S1 (`sas2308_idle_spindown`, default 0) | glue `S1-spindown` (idle stop not sent; START 1, LOEJ, POWER CONDITION, knob 1, system sleep, unknown state, halt: sent) | `pmset -a disksleep 1`: tunable 0 drives keep spinning after 3 min idle, tunable 1 they spin down; one sleep/wake; one restart; regression on 5 drives |
| **S2. Spin only while in use + `sas2308ctl spindown` (feature request)** | S2 mode 2 (an option since EJ), S2 grace (`sas2308_spindown_grace` 900 s), S2 spindown | glue `S2-in-use`, `S2-grace` | merged with EJ (not installed on its own); mode 2 not run on hardware |
| **EJ. Never spin down for idleness, Eject spins the drive down (user decision, after S2)** | EJ (default `sas2308_idle_spindown` 0 again; an eject's stop is sent in modes 0 and 2), S2 spindown = macOS's eject | glue `EJ-eject` | disksleep 1: mounted and unmounted drives keep spinning; `diskutil eject` and `sas2308ctl spindown`: stop passed, spin-up on the next read, Start_Stop_Count + 1, mount and read/write back; sleep/wake, restart; SMART 192 unchanged; regression on 8 drives |

Comment-only changes (no behaviour change) ride with whichever batch touches the file: listed in the last table.

**Batch F rationale** (added after batches A-E were merged, from the question "spin-down should finish
within 30 s, right?"). macOS's halt watchdog (`PEHaltRestartInternal`, 30 s, "Halt/Restart Timed
Out"; the 03:17 panic in `2026-11-final.md` section 5) is armed before the priority power interest
clients and the `systemWillShutdown` walk, and covers the whole halt. D15's 10 min backstop for
SAM's untimed START STOP UNIT, the 30 s + 10 s of any other command, 30 s reset TMs and a
diag-reset reinit cannot save a lost reply inside it: the watchdog panics first. H5 already fails
commands that arrive after our stop; the gap is a command in flight when the halt begins, or sent
during the halt before our stop. Fix: from the priority interest message (the earliest hook inside
the watchdog window, before any `systemWillShutdown`) every command gets at most 10 s left
(FreeBSD's shutdown figure: `mpssas_SSU_to_SATA_devices` times each SSU at 10 s; da's 1 h shutdown
SYNCHRONIZE CACHE and CAM_TIME_DEFAULT 0 = "SIM default" show FreeBSD has no comparable cap and no
watchdog, so D15's 10 min was our own number), every TM at most 3 s, and the reinit becomes H5's
bounded stop: 10 + 3 x 3 + 2 = 21 s worst case. Sleep keeps D15. Design: `design.md` "Batch F".
**Batch G1 rationale** (2026-11-21, after the final benchmark). The project's rule: keep FreeBSD's
mps/CAM behaviour unless there is evidence (a measurement, an Apple source line or xnu constraint, a
reproduced failure) for the deviation. The batch-G evaluations
(working notes, not in this repo) checked each open item against that rule. G1 takes the ones that need no
experiment: reverts with no evidence behind the deviation (D29 never ran; D22's `+error` showed
FreeBSD's internal chatter, not CAM's periph errors; D15a's 30 s had no source; D55b's 3 tries were
never used on hardware and Linux does not retry either; D58's arithmetic kept 7, not 8) and port
bugs fixed to FreeBSD's semantics (L3, L4, L7, L10, L11, L12, L13, L17, D43). G2 (D46, D21, D50,
D15b, D65 and the core-callout IO timer) waits for its experiments. Design: `design.md` "Batch G1".

**Batch G2 rationale** (2026-11-21). Same rule as G1: FreeBSD's behaviour is the default, and each
item's experiment either backs it or is the evidence for the deviation (recorded either way;
evaluations in working notes, not in this repo; the timer change is
`docs/perf-profile-20261121.md` proposal 2). Switches are port tunables (boot-arg / personality key,
`sas2308ctl set` at runtime), so the A/B runs needed no extra install. Design: `design.md` "Batch G2".

Hardware-only items: D34 (full-rate shutdown + verify + SYNCHRONIZE CACHE trace) after batch B; D75
(kIOMapPostedWrite A/B) after batch D; D29 decided by batch D's counter.

## Status

| Batch | Item | Status | Commit | Host test | Hardware |
|---|---|---|---|---|---|
| A | D1 | done | 2665587 | core `D1` | regression OK; SSD: still to run |
| A | D2 | done | f865cc8 | core `D2` | regression OK |
| A | D5 | done | 5870a52 | glue `D5` | still to run (dock unplug) |
| A | D3 | done | bb0b43e | glue `D3` | confirmed: 35 UAs (29/00) re-sent over 5 wakes, 0 F_FULLFSYNC errors |
| A | D8 | done | 1297470 | core `D8`, `D8-swap`, `D8-giveup`; glue `D8` | 5x sleep/wake: no eject, 0 generation changes; swap: still to run |
| A | D14 | done | 705347e | glue `D14`, `D14-cap` | still to run (disk pull) |
| A | D20 | done | 3715d37 | glue `D20a`; core `D20b`, `D20c` | regression OK |
| A | KEEP comments D28, D43, D46 | done | 51b85a5 | none (comments) | |
| B | D4 | done | 4e6f7bd | glue `D4` | IO held 35 s (`sas2308ctl hold`): 4 family timeouts, 4 re-arms, 0 IO timeouts, 0 TMs, 0 errors; a lost command times out once |
| B | D15 | done | ae0e70b | glue `D15` | regression OK |
| B | D44 | done | dcbfeef | none (IOKit class) | regression OK |
| B | D10 | done | 6101d7e | glue `D10`; mock OS-TM scenarios on `XPT_RESET_DEV` | recovery path on hardware (lost command: abort, LUN reset, target reset, reinit, IO re-sent, 0 errors); family TM requests: still to run (no unattended trigger) |
| B | D11 | done | bcafc51 | core `D11` | still to run (dock unplug: test 2 below) |
| B | D12 | done | c75c9b2 | core `D12` | 5x sleep/wake under IO (7 resumes incl. 2 dark wakes): all by the READY path, 0 diag resets, 0 re-IDENTIFYs; the 29/00 UNIT ATTENTION still came on every wake (7 per resume, all re-sent by D3) |
| B | D13 | done | a857195 | none (IOKit class) | forced diag reset: MSI control 0x81, address 0xFFFFF000, data 0x64, command 0x406 identical before/after; 1 restore, 0 failures; IO at full rate after |
| B | D19 | done | 784c280 | core `D19` | forced diag resets: restore called each time, IO fine |
| B | D18 | done | d916e4e | core `D18` | regression OK |
| B | D51 | done | d75366f | core `D51` | `kmutil unload` under 64K QD16 reads on all 7 raw drives: unloaded in ~2 s, no panic, no outstanding-IO or fault line, the reads end with errors; reload: 7 drives |
| B | D43 | done | 1414bbc | glue `D43` | `Lost completion kicks` 0 through all batch B tests |
| B | debug triggers (`sas2308ctl reinit/hold/lose`) | done | 3fc7ac5 | glue `debug` | used below |
| C | D37 | done | 3f35ef8 | core `D37` | regression OK |
| C | D36 | done | 99afc6c | glue `D36` | regression OK |
| C | D27 | done | a8a8407 | core `D27` (did not build before: new API) | attach log `IOCCapabilities: ...<...>` |
| C | D16 | done | 24ec6d2 | core `D16` | forced reinits under IO: OK (below) |
| C | D35, D67 | done | 7080f64 | core `D35` (3.1 M held-ccb checks before, 2501 after), `D67`; thin CAM test | forced reinit + lost command under IO: 0 errors |
| C | D17 | done | afe2fc5 | core `D17` | lost command: abort, LUN and target reset, 0 errors |
| C | D40 | done | 8914685 | core `D40` | attach OK (one HBA) |
| C | D42 | done | 4684bb2 | core `D42` | regression OK |
| C | D41 | done | b185c8a | core `D41` | regression OK |
| C | D39 | done | 23e5890 | core `D39` (did not build before: new API) | reinits under IO, two reboots and the D34 shutdown on the queue threads: OK |
| C | D33 | done | adb3ee7 | core `D33` | `mps.max_io_pages` from the personality: 257 segments, 2040 commands |
| C | D32 | done | 14ac253 | core `D32` | max transfer 1 MiB, task data 9.06 MiB (unchanged) |
| C | D21 | done | 7dc658f | none new (location only, same value; tests follow the move) | "Core OS commands" 2040 |
| C | D54 | done | 2cdc5a9 | core `D54` | lost command recovery requested its reinit through the wrapper |
| C | D78 | done | 08c020f | core `D78`; whole suite with QUEUE_MACRO_DEBUG_TRASH/ASSERTIONS | n/a (host only) |
| C | D6 | done | b8e6b94 | core `D6` (2 page reads during a stuck IDENTIFY; 1000 reads -> 0 mappings) | see "Batch C hardware results"; M5c retries / re-add slots kept: still to run, test 8 |
| C | KEEP comments D38, D65, D69, D17 | done | 0e8927d, 7080f64, afe2fc5 | none (comments) | |
| D | D52 | done | 9620c91 | `test/plist_test.sh` | loads on 0x0087 (the other two ids: no card to test) |
| D | D48 | done | 5340603 | none (IOKit class) | registry IOMinimumSegmentAlignmentByteCount = 1 |
| D | D30 | done | fe1ea5b | core `D30` | regression OK; faults would show in mps_periodic (none) |
| D | D31 | done | a0fbc9b | core temperature `D31` | temperature read every 30 s, 0 timeouts / failures; IOC 61 C |
| D | D29 (counter) | done | dd50951 | core `D29` | **"Event acks sent" 0 over 4 boots (25 events each), recovery tests included: D29 decided for boot; hot-plug: still to run, test 10** |
| D | D55 (counter) | done | e322813 | core `D55` | "KASSERT hits" 0 through every batch D test |
| D | D46 | done | 1f30ae6 | glue `D46` | regression OK (SAM sends SIMPLE) |
| D | D45 | done | 35d0029 | glue `D45` (did not build before) | random 4K QD32 1767 / 1762 / 1763 IOPS vs 1765 before (batch C): unchanged |
| D | D47 | done | 01f164a | glue `D47` (zero-length part failed before; the rest did not build) | "Tasks waiting for a mapping" 0 (no IOVA shortage seen) |
| D | D7 | done | b121c7f | core `D7` (failed at the 65th device before), glue `D7`, targets 1024 ids | MaxTargets 1024 (+ 0 volumes): initiator id 1024, the 7 drives keep ids 0-6 |
| D | D9 | done | 987f693 | glue `D9` (failed before) | no SATA SSD attached: still to run, test 9 |
| D | D73, D74 | done | bc5bfea | none (IOKit) | every DMA allocation and config page map succeeded (14 page 0 reads, temperature, 0 failures) |
| D | PCIe Link property (feature request) | done | 6d5b57a | `kext_pcie_test` | **8.0 GT/s (Gen3) x4** of the card's Gen3 x8; MPS **128** B (supports 4096), MRRS 512 B; same after forced reinits |
| D | D75 (switch for the A/B, then the default) | done | fb15323, ed91e3b | none (IOKit) | A/B below: posted writes faster on the host-bound load, 0 errors -> default (ed91e3b) |
| D | KEEP comment D46 (no periodic ORDERED) | done | 51b85a5, 1f30ae6 | none (comment) | |
| E | D22 | done | 9e2b9ed | core `D22` (default mask; no event dumps / Doorbell lines at attach and reinit) | see "Batch E hardware results" |
| E | D24 | done | aa63b9c | core `D24` (FAULT type, one precedence, surprise removal ERROR) | see "Batch E hardware results" |
| E | D23 | done | b98a54d | glue `D23` (core-refused IO: one line; xinfo race; no async lines) | see "Batch E hardware results" |
| E | D25 | done | 079715a | glue `D25` (did not build before: new API) | see "Batch E hardware results" |
| E | D26 | done | e5733f1 | core `D26` (handles 0x09/0x11 shared a window before) | see "Batch E hardware results" |
| E | D49(b) + KEEP comment D49(a) | done | 89b4e62 | core `D49` (MUR ignored at sleep: counted, said) | see "Batch E hardware results" |
| F | F1 (halt timeout cap, new) | done | 301d0d8 | glue `F`, `F-new`, `F-tm`, `F-pending-tm`, `F-reinit` (all failed before) | see "Batch F hardware results": lost read capped and failed 10.15 s after the halt began, stop instead of reinit, 0 diag resets; mounted restarts clean, no panic, volume OK |
| G1 | D29 (revert: no EVENT_ACK) | done | ee85cb9 | core/port: no ack on discovery, hot-add, reinit, removal | see "Batch G1 hardware results" |
| G1 | D22 (revert: default info,fault; stand-ins info,error) | done | cc9f637 | core `D22`, level presets | see "Batch G1 hardware results" |
| G1 | D15a (revert: 60 s Read/Write Time Out) | done | de1a7b2 | `plist_test.sh` | see "Batch G1 hardware results" |
| G1 | D55b (revert: one reinit attempt) | done | 512fa5b | mock reinit failure / nested fault, `D19`, port `TestReinitFailure` | see "Batch G1 hardware results" |
| G1 | D58 (reserve counted from FreeBSD's reqs: 2039) | done | 432549f | glue: max tasks == SIM openings − 8 | see "Batch G1 hardware results" |
| G1 | L12 (bad free: KASSERT, leak) | done | 9d2fe75 | core `L12` | KASSERT hits 0 |
| G1 | L13 (realloc targets: allocate first) | done | 650322a | core `L13` | none (needs a firmware change across a reset) |
| G1 | L4 (devq freeze fallback entry) | done | 4c304d9 | core `L4` | none (allocation failure) |
| G1 | L11 (callout handler marker + KASSERTs) | done | eac1298 | core `L11` | KASSERT hits 0 |
| G1 | L3 (held batch visible to cancel/flush) | done | 39ead7b | core `L3` | none (latent) |
| G1 | L17 (INTx fallback = mps_intr) | done | 67eb2d7 | glue `L17` | none (MSI only behind Thunderbolt) |
| G1 | L7 (glue lock until free(); snapshots under it) | done | 0d7fc95 | glue `L7` | see "Batch G1 hardware results" |
| G1 | L10 (hotplug on its own taskqueue thread) | done | ddddb5a | glue `H1`, `L10` | see "Batch G1 hardware results"; hot-plug: still to run, test 13 |
| G1 | D43 (TM pump only in the gate; timer detects only) | done | bca741f | glue `D43` | see "Batch G1 hardware results"; hot-plug: still to run, test 13 |
| G2 | D46 (da's periodic ORDERED tag; `sas2308_send_ordered`, default 1) | done | c760612 | glue `D46-otag` | A/B: no cost, no effect on the H pattern; kept on (see "Batch G2 hardware results") |
| G2 | D21 (one normal frame kept, not 8: 2046 OS tasks) | done | cbdc3eb | glue `D21` (FreeBSD's sizing loses the hot-add under a full pool; 1 or 8 frames do not) | regression; hot-add under a full pool: still to run, test 15 |
| G2 | D15b (untimed START STOP UNIT 60 s) | done | ca24b59 | glue `D15` | spin-up measured 10.5-11.7 s |
| G2 | D15b (halt cap 15 s, not 10 s) | done | 3679398 | glue `F`, `F-new`, `F-spinup` | restarts with spun-down disks 17-18 s on 21A171AA, no panic |
| G2 | wake log ("IOC in unexpected reset state" INIT on resume) | done | acc56e5 | mock `G2-wake` | see "Batch G2 hardware results" |
| G2 | D50 (no MUR at shutdown; `sas2308_shutdown_stop`, default 0) | done | 3a2a62b | mock `D50` | 7 restarts without the MUR: next attach READY, no DART line; power off: still to run, test 14 |
| G2 | IO timer (core callout; `sas2308_family_timer`, default 0) | done | 8df1186 | glue `C1`, `C1-family`, `C1-backstop`, `D4`, `D43` | F1 4K point +7.2 % (7 drives QD2), +14.5 % (QD1) |
| G2 | D65 (no per-device cap, KEEP-WITH-EVIDENCE) | done | bee8d0b | none (comment) | QD1 drive unchanged next to QD 1024 / 2000 |
| G2 | build fixes (Mac build) | done | ee94a31 | n/a | n/a |
| vendor | FreeBSD release/14.5.0 import (2 upstream commits, 3 lines, no conflicts, no new deviation) | done | 9c0a5f9, 72654ac | all host tests | smoke test on 830E83B5 (see "FreeBSD 14.5.0 import hardware results") |
| S1 | S1 (no idle spin-down; `sas2308_idle_spindown`, default 0: SAM's idle stop completed here) | done | f1cc568 | glue `S1-spindown` | tunable 0: first read 0.23-0.30 s after SAM's idle stop (5 stops completed here); tunable 1: 10.9-11.5 s (see "Batch S1 hardware results") |
| S2 | mode 2: SAM's idle stop sent only to a disk not in use (IORegistry walk, IOService::isOpen); the default only on the S2 branch, an option since EJ | done (option) | f8e8901 | glue `S2-in-use` | mode 2 not run on hardware (EJ chosen instead) |
| S2 | `sas2308_spindown_grace` (900 s after attach, mode 2 completes the idle stop here) | done (option) | d8c495e | glue `S2-grace` | not run (mode 2 only) |
| S2 | `sas2308ctl spindown <diskN\|all>`: was camcontrol stop through the shutdown-SSU path (d055c1a), then ATA STANDBY IMMEDIATE (3c60faa); both left the disk answering NOT READY 04/02 (SAM sends no START): now macOS's eject (DADiskEject), the kext passes its stop | done | d055c1a, 3c60faa, 5a9e440 | none (user tool; the kext part is EJ's) | spin-up 7.52-7.69 s on the next read, Start_Stop_Count + 1, mounts (see "Batch EJ hardware results") |
| EJ | default `sas2308_idle_spindown` 0 again; an eject's idle-type stop sent in modes 0 and 2 (SAM still Active and no media reported / driver open for DKIOCEJECT), "Stops passed (ejected)" | done | 5d805aa, 515295d | glue `EJ-eject` | eject: stop passed, first read 7.5-7.6 s, Start_Stop_Count + 1; idle: 8 stops at SAM state 3 completed here, first reads 0.02-0.24 s (see "Batch EJ hardware results") |

### Batch A hardware results

Build **DDA7E6AD** (fix-batch-A bb3a24c), installed 2026-11-20 18:23, 7 drives, 0 config page failures.

- **D3 + D8, 5x sleep/wake under IO** (`pmset sleepnow` + `pmset schedule wake` 60 s later):
  `~/hwA/fsyncw` writing 1 MiB + F_FULLFSYNC in a loop on both mounted JHFS+ test volumes, `sasbench`
  4K random reads QD8 on the other 5 drives. Each wake: one diag reset (reinit 496 ms), and every
  drive answered the first command with **UNIT ATTENTION 29/00**: `Unit attentions after reset
  retried` 7 per wake, 35 in all, each re-sent (log `UNIT ATTENTION 29/00 after our reset,
  re-sending the command`). **0 F_FULLFSYNC errors, 0 write errors**, both volumes stayed mounted
  through all 5 cycles, `diskutil verifyVolume`: both OK. 0 targets destroyed, 0 identity changes,
  0 IO timeouts / TMs / recoveries, no Error or Fault log lines. D3's symptom is confirmed real
  (29/00 after every wake); before this batch each of those 7 would have reached SAM. The D8
  keep-identity path did not trigger (every IDENTIFY answered: the drives stay spun up across a
  Mac sleep, the dock has its own power); it is covered by the host tests and test 3 below.
- **Regression** (drives unmounted): random 4K read QD32 all 7, 60 s: **1770 IOPS** (before 1748);
  seq 1 MiB read QD2 all 7, 60 s: **1945.5 MB/s** (before 1941.5); verify 64 KiB random QD64,
  256 MiB per drive: **0 mismatches, 0 errors**. `sas2308ctl stats` after all: 0 timeouts, 0 TMs,
  0 recoveries, 0 held timeouts, 0 stale/bad replies; Reinits/Diag resets 5 (the 5 wakes); IOC 61 C.

### Batch B hardware results

Build **9E6663B6** (fix-batch-B 3fc7ac5 code), installed 2026-11-20 19:17, 7 drives, 0 config page
failures, IOC 57 C. Debug triggers: `sas2308ctl reinit | hold <ms> | lose <n>`.

- **D12, 5x sleep/wake under IO** (batch A's harness in `~/hwB`: F_FULLFSYNC writers on the two
  JHFS+ volumes, 4K random reads QD8 on the other 5): 7 suspend/resume pairs (5 cycles, 2 extra
  around Thunderbolt dark wakes). **Every resume took the READY path** (log `resume: controller
  reinitialized without a reset`; `Resumes without reset` 7, `with diag reset` 0, `Diag resets` 0).
  0 F_FULLFSYNC/write errors, volumes stayed mounted, `verifyVolume` OK on both, 0 IO timeouts /
  TMs / recoveries, 0 re-IDENTIFYs or identity changes. Finding: **all 7 drives still answer the
  first command after every wake with UNIT ATTENTION 29/00** (49 = 7 per resume, all re-sent by D3):
  the link reset comes from the suspend's MUR / the port enable, not from the diag reset, so D3
  stays essential. `Err check condition` 35 per wake, as in batch A (unchanged).
- **D13 / D19, forced diag reset** (`sas2308ctl reinit`): PCI command 0x0406, MSI control 0x0081,
  address 0xFFFFF000, data 0x0064 before and after; `PCI state restores` 1, failures 0; random 4K
  QD32 on two drives right after: 376 IOPS, 0 errors (interrupts work).
- **D4, IO held 35 s** (`sas2308ctl hold 35000` under QD1 reads): 4 family timeouts, 4 re-arms
  (`family timer after 0 ms with the IOC (held), re-armed`), 0 IO timeouts, 0 TMs, 0 errors, the
  held read completed after 35.0 s. The real SCSIParallelTimer accepts a re-arm from HandleTimeout.
- **Lost command** (`sas2308ctl lose 1` under QD1 reads): one IO timeout 30 s after the post (the
  re-armed task timed out exactly once), ABORT_TASK, LUN reset, target reset (nothing returned, as
  expected for a command the IOC never saw), reinit; the READ re-sent (D20b) and completed: 0 errors,
  max latency 38.7 s; 7 UAs re-sent after that reinit.
- **D51, unload under IO** (all 7 raw drives, 64K random reads QD16): `kmutil unload` rc 0 in ~2 s,
  log `stopping controller`, `mps_free entered/exit`, no error/fault lines; the reads ended with
  errors (device gone), no panic; `kmutil load` rc 0, 7 drives back.
- **Regression** (after the reload, drives unmounted): random 4K read QD32 all 7, 60 s: **1768
  IOPS**; seq 1 MiB read QD2 all 7, 60 s: **1945.4 MB/s**; verify 64 KiB random QD64, 256 MiB per
  drive: **0 mismatches, 0 errors**. `sas2308ctl stats` after: 0 timeouts, 0 TMs, 0 recoveries,
  0 reinits, 0 stale/bad replies, 0 lost kicks, 0 transient all-ones reads; IOC 58 C.

### Batch C hardware results

Build **6DD7CE42** (fix-batch-C 0e8927d), installed 2026-11-20 20:33, 7 drives, 0 config page
failures, IOC 59 C. Rollback: `~/sas2308-rollback-9E6663B6` (batch B build).

- **D6 on hardware**: discovery read the 14 SAS Device Page 0s (7 drives + parents) and every
  IO Unit Page 7 (temperature, every 30 s) into `malloc`'d pages mapped at load: 0 failures, 0
  retries. No mapping leaks: `ioclasscount` IODMACommand 21525 and IOGeneralMemoryDescriptor
  960/959 unchanged across 13 temperature reads and the regression; `ioalloccount` IOMalloc
  223648 K -> 223645 K. Kext memory (`Controller Limits`): heap 1.21 MiB, DMA 3.08 MiB (the
  1 KiB config buffer gone), task data 9.06 MiB (2040 x 4656 B: +8 B per task for D32's
  segment list after the ccb; 257 segments, as before). System wired after boot 77022 pages
  (16 KiB), 77628 after the tests (batch B build, 1 h uptime: 75527): no kext share in the
  growth (IOMalloc flat).
- **D16 / D35 / D39 / D54 / D17, forced recovery under IO** (64K random reads QD16 on all 7 raw
  drives, 70 s; `sas2308ctl reinit` at 15 s, `sas2308ctl lose 1` at 35 s): 2 reinits (diag
  resets, PCI restores 2), the lost command timed out once and went abort -> LUN reset ->
  target reset -> reinit (`controller reinit requested: ...` through the D54 wrapper), 7 UAs
  re-sent (D3); **0 sasbench errors, 0 mismatches**, max latency 39.1 s (the lost command),
  1082 IOPS overall; 41 tasks held during the recoveries, all released (held 0 after).
- **D34, shutdown at full write rate** (run on this build): region 3-4 % of each drive, 4 GiB,
  written and verified with seed 11; then a seed-22 writer (1 MiB, QD4, all 7 raw drives) and
  `shutdown -r now` 6 s later. After the boot every 4 KiB block of every drive holds seed 22 or
  seed 11 (blocks not holding 22 + blocks not holding 11 = 1048576 exactly on all 7; 2.84-2.89
  GiB of seed 22 per drive), 0 read errors, every logged mismatch `stale-data(old seed)`: no
  torn, misdirected or corrupted block. The SYNCHRONIZE CACHE trace could not be taken: with
  `log config ... persist:debug` and trace level set 4 s before the shutdown, nothing of the
  kext's shutdown path reached the log store (logd stops before `systemWillShutdown`; only the
  mask-change line 4 s earlier was persisted). The dock keeps the drives powered across the
  Mac's restart, so a cache flush is not what this run can prove; data integrity it does.
- **Regression** (drives unmounted): random 4K read QD32 all 7, 60 s: **1765 IOPS**; seq 1 MiB
  read QD2 all 7, 60 s: **1944.6 MB/s**; verify 64 KiB random QD64, 256 MiB per drive: **0
  mismatches, 0 errors**. `sas2308ctl stats` after: 0 timeouts, 0 TMs, 0 recoveries, 0 reinits
  (this boot), 0 stale replies, 0 lost kicks, 0 transient all-ones reads, 0 config page
  failures/retries; no error or fault log line in this boot.

### Batch D hardware results

Builds **EBB85578** (fix-batch-D fb15323, BAR1 non-posted by default) and **509E19D3** (ed91e3b,
posted writes by default; the batch's final build), installed 2026-11-20 21:27 and 21:55, 7 drives,
0 config page failures, IOC 61 C. Rollback: `~/sas2308-rollback-6DD7CE42` (batch C build, master).
`tools/diff-vs-freebsd.sh -s`: 1281 added / 3595 removed lines (batch C: 1198 / 3836; the
use_phynum block, the SSU functions and the INQUIRY/SSU_refcount lines are FreeBSD's text again,
D31's timeout handler and the D7 table hooks are new XNU lines).

- **PCIe link** (`sas2308ctl show`, "PCIe Link", from the card's own config space): Link Status
  0x1043 = **8.0 GT/s (Gen3) x4**; Link Capabilities 0x00400483 = the card can do Gen3 x8 (the
  JHL7440 tunnel is x4); Device Control 0x291F = **Max Payload Size 128 B**, Max Read Request Size
  512 B; Device Capabilities 0x10008025 = **MPS supported 4096 B**; 128 B in use, as expected below the
  JHL7440 bridge (15ef), which supports 128 B only (its own registers not read here). Unchanged after 2 forced diag resets (re-read with "Controller Limits").
- **D29**: "Event acks sent" **0** on every boot (4 boots this batch, 25 events each: port enable
  discovery of 7 drives) and through the recovery test below; the 9207's IT firmware never set
  AckRequired so far. Hot-plug events still to see: still to run, test 10, then the ack code goes
  (FreeBSD) if still 0.
- **D31 / temperature**: polled every 30 s ("IOC temperature reads" 2 right after boot, 9 after
  ~5 min, 21 after the A regression), "IOC Temperature (C)" property 61 C, 0 failures, 0 timeouts.
- **D75, BAR1 mapping A/B** (boot-arg `sas2308_posted_write=1` on EBB85578, then the personality
  default on 509E19D3): sequential 4K reads QD32 on all 7 (drive read-ahead, host-bound):
  non-posted **173081, 173343** IOPS, posted **177563, 177153** (+2.5 %); random 4K QD32 (drive-bound)
  1767 vs 1762 / 1763; seq 1 MiB 1944.9 vs 1945.5 / 1945.3 MB/s; verify 0 errors, 0 mismatches
  either way. Posted writes are the default now (`sas2308_posted_write` 1 in Info.plist;
  boot-arg `sas2308_posted_write=0` reverts).
- **Forced recovery with posted writes** (509E19D3; 64K random reads QD16 on all 7, 70 s;
  `sas2308ctl reinit` at 15 s, `lose 1` at 35 s): 2 reinits (diag resets, PCI restores 2), the lost
  command went abort -> LUN reset -> target reset -> reinit, 14 UAs re-sent, held 255 released;
  **0 errors, 0 mismatches**, 1080 IOPS (batch C: 1082), max latency 39.0 s (the lost command).
- **D45 / D46 / D47 / D48 / D7 / D73**: random 4K QD32 1763 IOPS (batch C 1765: the slot table's
  scans cost nothing visible at this rate); registry IOMinimumSegmentAlignmentByteCount 1; SCSI
  Initiator Identifier 1024, MaxTargets 1024, drives on ids 0-6; "Tasks waiting for a mapping" 0;
  every DMA allocation and bufmap succeeded (no bounce); "KASSERT hits" 0 throughout.
- **Regression** (509E19D3, drives unmounted): random 4K read QD32 all 7, 60 s: **1763 IOPS**; seq
  1 MiB read QD2 all 7, 60 s: **1945.3 MB/s**; verify 64 KiB random QD64, 256 MiB per drive: **0
  mismatches, 0 errors**. `sas2308ctl stats` after: 1 IO timeout, 1 recovery, 2 reinits, 1 target
  reset (all from the forced recovery test), 0 temperature timeouts, 0 stale replies, 0 lost kicks,
  0 transient all-ones reads, 0 config page failures, 0 KASSERT hits.
- **Not tested here**: D9 (no SATA SSD: still to run, test 9), D52's other two ids (no such card),
  D7 beyond 64 devices (no expander: host tests only).

### Batch E hardware results

Build **6542C39F** (fix-batch-E 89b4e62), installed 2026-11-20 22:29, 7 drives, 0 config page
failures, IOC 54 C; `SAS2308DebugMask` 0x23 = info,fault,error. Rollback:
`~/sas2308-rollback-509E19D3` (batch D build, master a71e38a). `tools/diff-vs-freebsd.sh`: common
7383 unchanged, removed 3595 unchanged, added 1281 -> 1293 (D49's said-and-counted sleep stop in
the XNU-only `mps_xnu_suspend`, D26's member in `struct mpssas_target`; mps.c's default mask line is
FreeBSD's again plus `MPS_ERROR`). Persisted lines counted with `log show --info --debug --style
ndjson` (script `~/hwE/count.sh`).

- **Boot**: 0 driver lines persisted from attach and discovery. macOS loses the first ~1 min of
  kernel os_log after boot (the check ran 29 s after boot; it was the same on every earlier build),
  so attach-time lines are not a measure; the two `debug mask ... set from userland` lines of
  `sas2308ctl level debug` / `level info` were the only lines since boot after 6 min.
- **5 min idle** (drives spinning, temperature polled every 30 s): **0 lines** of any type (batch D
  build: 0 too, the log-noise change had already removed the TEST UNIT READY lines).
- **Forced recovery under IO** (random 4K QD8 on all 7 raw drives, 130 s; `sas2308ctl lose 1` at
  5 s, `sas2308ctl reinit` at 80 s), same script on both builds:

  | build | persisted lines | Default | Error | Fault | by category |
  |---|---|---|---|---|---|
  | 509E19D3 (batch D, old default) | **1164** | 1156 | 8 | 0 | event 660, recovery 297, init 104, mapping 84, info 11, error 8 |
  | 6542C39F (batch E) | **31** | 25 | 6 | 0 | info 12, recovery 9, error 6, init 4 |

  Batch E's 31 lines carry the whole story: the debug triggers (info), FreeBSD's
  `Command timeout`, `Sending abort`, `Aborting command`, `Sending logical unit reset`, `... sending
  target reset`, `Target reset complete ... resetting controller`, `Reinitializing controller` x2,
  `reinit done in N ms` x2, `Firmware` / `IOCCapabilities` per reinit, the IO the resets terminated
  (5 error lines + one `similar IO error lines suppressed`, per target, D26). Gone: the per-event
  field dumps, startup refcount, PHY, `Target id added`, `killed by a reset: re-sent` (83) and
  `UNIT ATTENTION ... re-sending` (14) recovery-trail lines (opt-in: `sas2308ctl mask +recovery`).
  Outcome identical on both builds: 1 IO timeout, abort -> LUN reset -> target reset -> reinit, 2
  diag resets, **0 IO errors, 0 mismatches**, 1084 IOPS (old 1087), max latency 38.9 s (the lost
  command).
- **D24**: no FAULT line in any window (no IOC fault, nothing disabled); the FAULT path (IOC fault,
  failed reinit) and the ERROR surprise removal are host-tested; dock unplug: still to run, test 11.
- **D25**: no device-less line occurred (no KASSERT hit, `KASSERT hits` 0); host-tested.
- **D49(b)**: `Sleep stop failures` 0 (published); no sleep cycle in this batch (the wake path is
  unchanged except for the failure lines; batches A/B ran 10 wakes on it).
- **Regression** (6542C39F, drives unmounted): random 4K read QD32 all 7, 60 s: **1765 IOPS**; seq
  1 MiB read QD2 all 7, 60 s: **1945.4 MB/s**; verify 64 KiB random QD64, 256 MiB per drive: **0
  mismatches, 0 errors**; **0 driver lines persisted** during the regression. `sas2308ctl stats`
  after: 1 IO timeout, 2 reinits / diag resets, 1 target reset (all from the forced recovery
  test), 0 stale replies, 0 lost kicks, 0 transient all-ones reads, 0 config page failures, 0
  KASSERT hits, 0 sleep stop failures.
- **Not tested here**: hot-plug line counts (still to run, test 11).

### Batch F hardware results

Build **085DA9D6** (fix-batch-F), installed 2026-11-20 23:19, 7 drives, 0 config page failures,
IOC 53-59 C, no "no priority power interest" error line (the halt notifier registered). Rollback:
`~/sas2308-rollback-6542C39F` (batch E build, master ba4a02f). `tools/diff-vs-freebsd.sh`: common
7383 -> 7380 (the three TM `callout_reset` lines and the IO timeout line carry the cap), added
1293 -> 1360 (`mpssas_xnu_halt_caps` in mps_sas.c, the two reinit hooks in mps.c; the halt
functions themselves are in the XNU-only mps_xnu.c). Scripts and outputs: Mac `~/hwF`.

- **Deterministic halt test** (`~/hwF/hwF-halt.sh`; drives unmounted): `sas2308ctl lose 1`, a 4K
  raw read of disk4 (target 0) is lost at 23:25:38.3; 3 s later `sas2308ctl halt` (the same
  `mps_xnu_halt_begin` the restart's priority interest calls): `halt: 1 commands outstanding, 1
  capped at 10 s`. At **+10.00 s** `Command timeout ... 40000 set` (the ccb keeps its original
  30 s + 10 s figure; only the callout moved), abort, LUN reset, target reset (IOC answers each in
  ~25 ms, the command stays lost), `halt: stopping the controller instead of a reinit`, `suspended:
  IOC stopped` (MUR acknowledged, 53 ms). The read failed **10.15 s after the halt began** (13.17
  s after submit; without F: the family timer at 30 s, then a diag-reset reinit). A second read
  failed in 0.02 s (stopped controller, H5). Counters: IO timeouts 1, Halt timeouts capped 1, Halt
  recovery stops 1, Reinits 0, **Diag resets 0**. `shutdown -r now` afterwards: request -> kernel
  boot 12 s, no panic, attach clean on the stopped IOC, 7 drives.
- **(b) normal restart with a mounted volume** (H5drive6 on disk9, 256 MiB synced + 64 MiB
  left dirty): request 23:22:59 -> kernel boot 23:23:11, **12 s**; no panic; both files verify
  (sha256) after the boot.
- **(a) `lose 1` armed right before `shutdown -r now`, mounted volume**: the next SCSI IO lost was
  SMID 1630 for **target 5 = disk9, the mounted volume**, at 23:24:00.868. Request 23:24:00 ->
  kernel boot 23:24:47, **47 s** (35 s more than (b)); no panic, no new report except the routine
  shutdown stall; both files verify. The 35 s are the lost write recovered by the normal path (30 s
  family timer, abort -> LUN -> target reset -> reinit) while the unmount waited for it, i.e.
  **before** the halt began: logd stops at 23:24:01, so the recovery itself was not persisted. This
  is the pre-halt case, which never had a watchdog; it shows the mounted-volume shutdown survives a
  lost command. A command lost **inside** the halt cannot be produced by a restart on this setup:
  on restart nothing sends IO in the halt (SAM marks "Power Off" and sends its START STOP UNIT only
  for power off, and D9's SSU goes only to SATA SSDs, none attached); that case is covered by the
  deterministic test above (same entry point) and by the host tests; power off: still to run, test
  12.
- **Shutdown stall reports** (all of `/Library/Logs/DiagnosticReports/shutdown_stall_*`, decoded
  with `spindump -i`, `~/hwF/stalls.sh`): one per restart, 31 since 2026-11-15, including 11 from
  2026-11-15..17 before the kext existed; the 20 of 2026-11-20 decoded, two of them (00:08, 05:35)
  with no SAS2308HBA loaded.
  Each is launchd's routine sample ("shutdown stall", sampling starts 2 s into the userspace
  shutdown, 1 s long) taken while processes are still exiting (AppleCentauri*, DriverKit drivers,
  softwareupdated, duetexpertd, spindump itself); **0 frames in IOSCSI*/IOStorage or our kext in
  any of the 20**, our work-loop thread idle. None is a hang: the restarts took 12-47 s request to boot.
  22:28 = batch E's install reboot (509E19D3 shutting down), 23:18 = this batch's approval reboot
  (6542C39F shutting down, drives unmounted, nothing injected), 23:23 / 23:24 / 23:26 = tests (b),
  (a), and the reboot after the halt test. They are not the 03:17 picture (threads asleep in
  `commandSleep` for 30 s inside the halt, then "Halt/Restart Timed Out"); the 03:16 stall report a
  minute before that panic (build 386F7D9D) shows only a turnstile wait on userfsd, also no storage
  frames.
- **Regression** (085DA9D6, drives unmounted): random 4K read QD32 all 7, 60 s: **1772 IOPS**; seq
  1 MiB read QD2 all 7, 60 s: **1944.9 MB/s**; verify 64 KiB random QD64, 256 MiB per drive: **0
  mismatches, 0 errors**. `sas2308ctl stats` after: 0 IO timeouts, 0 reinits / diag resets, 0
  halt caps / stops (fresh boot), 0 config page failures, 0 KASSERT hits, 0 lost kicks.

### Batch G1 hardware results

Build **430A4B8A-5B0C-341E-8CBF-F2388BF576E4** from bca741f (no build fixes needed; 5626d4e after it is
docs only). Raw outputs: Mac `~/hwG1`.
- **Default log:** mask 0x3 (info,fault), level info. 5 min idle: **0 lines**.
- **Forced recovery under IO** (random reads on all 7): `lose 1`: the family timeout fired at
  **60.0 s** (D15a), abort → LUN reset → target reset → reinit **1118 ms** (one attempt, D55b); the lost
  IO completed after 68.9 s at most, IO stalled about 8.5 s per reinit, **0 application errors**.
  `sas2308ctl reinit`: 1169 ms. "TM waits in gate" **0** (D43), "Lost completion kicks" 0, "KASSERT
  hits" 0, "Reinit failed attempts" 0. The default log showed the recovery (FreeBSD's info|recovery
  lines), no "NULL handle" chatter.
- **Sleep/wake under IO:** 2 suspend/resume cycles (one a dark wake), resume without a diag reset
  (343 / 348 ms), 0 timeouts, 0 errors. **Follow-up (G2):** each wake logs 4 FreeBSD
  `mps_transition_ready` "IOC in unexpected reset state" lines at FAULT, now visible at the default
  mask, although a RESET state after sleep is expected; the port's resume path should treat it as
  expected (not a fault there) without changing FreeBSD's text elsewhere.
- **Restart with a mounted JHFS+ volume** (G1TEST on disk9 drive6): 13 s from request to boot, no
  panic (the routine launchd shutdown stall report only), verifyVolume OK, **202/202 files verify**.
- **SMART:** `smartctl -a` on all 7 after the install reboot: all **PASSED**. disk4 drive1 and disk5
  drive2, which reported "SMART Status: Not Supported" after an earlier `smartctl -x`
  (entry 76), recovered with the reboot. Not a driver issue as far as seen: smartmontools' `-x` on
  macOS left the two nubs without SMART until reboot; reads were unaffected. disk9 rc 64 = one old
  entry in its error log.
- **Regression:** random 4K read QD32 all 7, 60 s: **1757 IOPS**; seq 1 MiB read all 7, 60 s:
  **1945.3 MB/s**; verify **0 mismatches, 0 errors**; 0 timeouts / reinits beyond the tests.

### Batch G2 hardware results

Before the install (build 430A4B8A, the G1 code), on build **4746DE60-BFFE-3991-B29A-822AD3810EBE**
(ee94a31) and on the final build **21A171AA-980F-3D71-9714-E9503E8B0A25** (3679398, the 15 s halt
cap); rollback copies `~/sas2308-rollback-430A4B8A`, `~/sas2308-rollback-4746DE60`. Raw outputs: Mac `~/hwG2` (scripts `pre.sh`, `exp1.sh`, `rs.sh`, `postboot.sh`, `reg.sh`)
and `~/sasbench/profile/<tag>` for the 4K point.
- **D65, one deep queue next to a QD1 drive** (430A4B8A, random 4K read, full LBA range): drive B at
  QD1 alone 84 IOPS, p50 11.5 ms, p99 20.5 ms, max 27.8 ms; next to drive A at QD 1024: 84 IOPS, p99
  20.0 ms, max 26.4 ms; next to A at QD 2000: 84 IOPS, p99 20.5 ms, max 49.0 ms. A: 203 IOPS at QD 1024
  (p50 4.98 s, max 5.75 s), 202 IOPS at QD 2000 (p50 9.70 s, max 10.6 s): Little's law, the IOC queues
  past NCQ's 32 without loss. 0 errors, 0 rejected tasks, 0 SCSI status errors, 0 timeouts. Decision:
  no per-device cap (KEEP-WITH-EVIDENCE): nothing for it to fix here, and the family allocates its
  controller-wide task pool before the SIM sees a task, so a SIM-side cap could not give CAM's
  per-device guarantee anyway.
- **D15b, spin-up from standby** (430A4B8A, `pmset -a disksleep 1`, SAM's power states down to
  standby): the first 4K read took 10.84-11.70 s on 7 drives at once, 10.50 s on one drive alone
  (the second read 1 ms). The 10.89 s / 10.99 s `Latency max` of earlier runs were spin-ups. The
  untimed START STOP UNIT gets da's 60 s (5x the slowest).
- **D46, ORDERED tag A/B** (4746DE60, `sas2308ctl set sas2308_send_ordered`, ABBA on one boot; H =
  1 MiB seq QD2 write 0-1.5 % on 4 drives + read 98-99.5 % on 3, with 64 KiB random 70/30 QD8 on all
  7 at 2-50 %, 180 s; then random 4K read QD32 all 7 full range, 120 s, 60 s after the writes):

  | send_ordered | H seq MB/s | H rand IOPS | H rand p99 / max | rr4K QD32 IOPS | rr4K max |
  |---|---|---|---|---|---|
  | 1 (run 1) | 1227.3 | 301 | 2.49 s / 2.589 s | 1400 | 0.848 s |
  | 0 (run 2) | 1232.6 | 285 | 2.49 s / 2.588 s | 1402 | 0.999 s |
  | 0 (run 3) | 1250.2 | 257 | 2.49 s / 2.595 s | 1403 | 0.985 s |
  | 1 (run 4) | 1224.0 | 282 | 2.49 s / 2.617 s | 1402 | 1.005 s |

  "Ordered tags sent" 70 per round with 1 (one per drive per 30 s of continuous IO, 7 drives x
  300 s), 0 with 0; 0 errors, 0 timeouts. No cost (throughput within run-to-run noise, -1.3 % to
  +0.2 %) and no effect on the H pattern's starvation either (its worst case, 2.6 s, is far inside
  the 15 s interval). Decision: FreeBSD's default 1 kept.
- **IO timer A/B** (4746DE60, `sas2308ctl set sas2308_family_timer`, F1 4K point `repro.sh`, order
  0 1 0 1 on one boot; 7 drives = 4K seq read QD2 on 7 drives, 3 x 30 s):

  | scheme | 7 drives QD2 (IOPS, p50) | 1 drive QD2 | 1 drive QD1 | 7 drives QD1 |
  |---|---|---|---|---|
  | 430A4B8A before the install (family timer) | 158,426 / 160,530 / 160,324 (82 us) | 37,272 | 18,526 | 111,437 |
  | core callout (0), run 5 | 166,971 / 171,529 / 171,213 (74-78 us) | 37,492 | 18,633 | 126,531 |
  | family timer (1), run 6 | 160,232 / 159,876 / 159,580 (82 us) | 37,194 | 18,546 | 110,626 |
  | core callout (0), run 7 | 173,161 / 172,972 / 173,129 (74 us) | 37,462 | 18,638 | 126,611 |
  | family timer (1), run 8 | 160,660 / 159,380 / 160,066 (82 us) | 37,287 | 18,494 | 110,561 |

  Core callout +7.2 % on 7 drives QD2 (mean 171.5k vs 160.0k), +14.5 % on 7 drives QD1, p50 82 →
  74 us; one drive (no gate contention) +0.5 %. The 11-20 ceiling (170.3k) is back. "Family
  timeouts" 0, "Family timer re-arms" 0, "Lost completion kicks" 0, "Tasks rejected" 0.
- **D50, restart without the MUR** (4746DE60, drives spinning, `shutdown -r now`): 12 s from the
  request to the next boot (G1 with the MUR: 13 s). Next attach: `IOC state at attach` **READY**
  (doorbell 0x10000000): the Thunderbolt link-down across the restart resets the IOC by itself,
  the MUR had nothing left to stop. No DART line in the kernel log or `dmesg`, "Diag resets" 0,
  "Reinits" 0, 7 drives, "Config page failures" 0. Decision: FreeBSD's no-stop kept. (The install
  restart, with the old kext's MUR, also attached READY.) Every later restart (6 on 4746DE60 and
  21A171AA) also attached READY with no DART line and 0 diag resets.
- **D15b, restarts with the disks spun down** (`pmset -a disksleep 1` until SAM had all 7 below
  its idle state, then `shutdown -r now`; request → `kern.boottime`): spinning 13 s (G1), 12 s
  (4746DE60), 12 s (21A171AA); spun down 17 s and 24 s with the 10 s halt cap (4746DE60), 18 s and
  17 s with the 15 s cap (21A171AA). The persisted log ends about 1 s after the request (logd stops
  first), so the command that waits is not visible; the extra 5-12 s and the 10.5-11.7 s spin-up
  are what the cap has to allow for. The SMART Start_Stop_Count was not usable as a witness (+1 or
  +2 per cycle depending on SAM's power-state path). Install restarts take 28-35 s (kernel
  collection rebuild) and are not counted. Decision: halt cap 15 s (26 s worst case inside the 30 s
  watchdog), second install (21A171AA).
- **Boot log persistence** (every reboot of this batch: the two install restarts and R2-R4, R6-R8):
  0 driver lines persisted for the boot, also not the `IOC state at attach` line logged during
  attach; the registry property `IOC state at attach` is what records it.
- **Sleep/wake under IO** (21A171AA, random 4K QD8 on all 7, `pmset sleepnow` + scheduled wake):
  2 suspend/resume cycles (one a dark wake), both without a reset (reinit 340 / 397 ms), 0 IO
  timeouts, 0 errors (14 UNIT ATTENTIONs after our reset re-sent, D3). Default log: 28 lines, **0
  fault-type lines and no "IOC in unexpected reset state"** (G1: 4 FAULT lines per wake).
- **Regression** (21A171AA): random 4K read QD32 all 7, 60 s: **1767 IOPS**; seq 1 MiB read all 7,
  60 s: **1945.2 MB/s**; verify 256 MiB per drive **0 mismatches, 0 errors**; 0 IO timeouts, 0
  recoveries, 0 diag resets, 0 KASSERT hits, 0 lost kicks, 0 rejected tasks; "Ordered tags sent" 86.

### FreeBSD 14.5.0 import hardware results

Build **830E83B5-D729-3439-B6D9-592E227B457C** (branch import-fbsd-14.5, merge 72654ac); rollback
copy `~/sas2308-rollback-21A171AA`. Raw outputs: Mac `~/hw145` (the G1 scripts with paths changed).
Upstream changed only a dprint string and loop-header whitespace, so this is a smoke test, not an
A/B.
- **Install:** VNC approval, reboot; 7 drives, 0 config page failures, temperature read OK.
- **SMART** `smartctl -a`: PASSED on all 7. drive6 (disk9) returns rc 64 for one old ATA error
  log entry (UNC at power-on hour 21639, 130 h before this test; the count was already 1 in G1).
- **Idle:** default level and mask (2, 0x3): 0 lines in 5 min, 0 with info+debug.
- **Forced recovery under IO** (random 4K QD8 on all 7, `lose 1`, then `reinit`): timeout at
  60.6 s, abort, LUN reset, target reset, reinit 1118 ms; forced reinit 1124 ms; 0 application
  errors; KASSERT 0, TM waits in gate 0, lost kicks 0.
- **Sleep/wake under IO:** 2 suspend/resume cycles (the sleep and a maintenance sleep before the
  full wake), both without a reset (350 / 340 ms), 0 errors, 28 default lines, 0 fault lines.
- **Restart with a mounted JHFS+ volume** (F145TEST on drive6, 256+64 MiB + 200 small files, one
  file dirty at shutdown): request to kernel boot 12 s, 0 driver lines in the shutdown window,
  `verifyVolume` OK, 202/202 shasums OK. The shutdownStall report and the kernelmanagerd
  "restricted lookup table" line come with every restart on this Mac (G1 had both).
- **Regression:** random 4K read QD32 all 7: **1764 IOPS** (G2 1767); seq 1 MiB read all 7:
  **1945.2 MB/s** (G2 1945.2); verify **0 mismatches, 0 errors**.

### Batch S1 hardware results

Rationale: drives that never spin down, with a tunable (the request: "never spin down ... it
should be param"); FreeBSD's da/CAM never stop a disk for idleness, so the default is FreeBSD's
behaviour and the deviation is macOS's SAM idle timer being cut off at the HBA. The HBA sees no
mounts: it applies to every drive behind it. Design: `design.md` "S1".

Build **95E0D6C9-AC58-399D-A2CB-370687C33590** (branch spindown-tunable, f1cc568), installed with
`mac/install.sh` (first real use; sudo), rollback copy
`~/sas2308-rollback-830E83B5`. Two drives were removed before this batch (drive4, drive5): 5 drives
(drive1, drive2, drive3, drive6, drive7). Raw outputs: Mac `~/hwS1` (`spin.sh LABEL KNOB
DISKSLEEP`: knob, `pmset -a disksleep`, 240 s idle with SAM's CurrentPowerState every 20 s, then one
timed 4K read at a random LBA per drive in parallel, then a second one).
- **(a) `sas2308_idle_spindown` 0, disksleep 1:** SAM went 4 → 3 (40 s) → 2 (80 s) → 1 (100 s) on all 5;
  `Idle stops completed here` 0 → 5 (one per drive, at the Standby step; nothing sent). First read
  **0.227-0.303 s**; SAM back to 4.
- **(b) tunable 1, disksleep 1:** the same steps; 5 × "START STOP UNIT (byte 4 0x00) sent" (CDB
  `1b 00 00 00 00 00`: SAM sends IMMED 0 here, not the IMMED 1 of the published source; the code
  ignores IMMED), counter unchanged. First read **10.862-11.483 s** (spin-up), second 0.008-0.018 s.
- **(c) control, tunable 0, disksleep 0 (SAM stays Active):** first read after 240 s idle
  **0.214-0.354 s**, second 0.007-0.014 s. So the ~0.23 s of (a) is the drives' own idle state
  (SATA drives behind SAT, firmware idle timers), not SAM or the HBA: with the tunable at 0 the
  drives stay spinning exactly as if SAM never idled them.
- **Side finding:** `diskutil info` (in `sasdisks.sh`) sends ATA PASS-THROUGH (SMART status), which
  brings SAM back to Active (START 1) and restarts its idle timer.
- **Sleep/wake** (tunable 0, disksleep 10, random 4K QD8 on all 5, `pmset sleepnow` + scheduled
  wake): 3 suspend/resume cycles (sleep + 2 dark wakes), all READY-path resumes without a reset
  (340-350 ms), 0 IO errors, 0 timeouts, 0 diag resets; the system state went SLEEP → AWAKE at the
  full wake (14:12:49.996, scheduled 14:12:50; the WillSleep line itself was not persisted, the
  state shows it arrived). SAM sent no START STOP UNIT at this system sleep (none sent, none
  completed here: counter 10 → 10).
- **Restart** (drives spinning, unmounted): request → kernel boot **13 s** (12-18 s before), the usual
  shutdownStall report, 5 drives back, 0 diag resets, 0 config page failures.
- **Regression (5 drives):** random 4K QD32 **1262 IOPS** (251-254 per drive; 14.5.0 import: 1764 / 7 =
  252), seq 1 MiB **1391.4 MB/s** (278 per drive; before 1945.2 / 7 = 278), verify 0 mismatches,
  0 errors; 0 IO timeouts, reinits, recoveries, KASSERT hits.
- Logging: `log config --mode persist:debug` for the subsystem during (b) to sleep, reset afterwards.

### Dock unplug, idle (by hand, 2026-11-21 18:47)

First real dock unplug (part of tests 2 and 7, without IO and without a mounted volume), build
95E0D6C9. The 5 drives were unmounted and stopped first (`sas2308_idle_spindown 1`, `pmset -a
disksleep 1`, SAM to standby on all 5), then the Thunderbolt cable was pulled, the dock powered
off and recabled. All in the same millisecond (18:47:59.337): `register 0x00 reads 0xffffffff and
the PCI config space does not answer: controller gone (surprise removal)`, `controller disabled`,
bus mastering off, targets 0-4 destroyed, `terminating (controller gone)`, `stopping controller`.
No `checking whether the controller is still there` line was seen first (the config read already
failed). Mac stayed up; the next boots attached 8 drives (7 known + drive8), state operational,
0 errors. SMART against the 09:32 baseline: Power-Off_Retract_Count unchanged on all 7 known drives
(no emergency retract), Power_Cycle_Count +1 each, 5/197/198 = 0, 187/188/199 unchanged.
- Follow-up (cosmetic): `stopping controller` then logs `Cannot transition IOC to ready` at fault
  level on hardware that is gone; expected there, should not be a fault.
- Still open: test 2 (unplug under IO with a mounted volume) and test 7's transient-blip half.

### Batch S2 hardware results

Rationale: feature request (spin only while in use, an immediate spin-down command); FreeBSD never
stops a disk for idleness, `camcontrol stop` is its manual equivalent (design.md "S2"). Built on
the Mac (FCE29EDC-ADAC-3CD7-8E14-97B601FD078D, `make check` clean), installed with mac/install.sh
(backup ~/sas2308-rollback-95E0D6C9, kmutil rc 27). Not approved yet: the legacy-VNC step of the
approval was not run, so 95E0D6C9 is still loaded and the
hardware tests (a)-(c) are not run. Done without the new kext: Spotlight (LOADING.md), SMART
baseline (192: 49 59 54 52 54 54 56 61 for drive6 drive2 drive7 drive8 drive3 drive5
drive4 drive1), and the new `sas2308ctl spindown all` against the old kext (8 disks found,
disk4 refused as mounted, the others "kext too old"; `disk4s2` and internal `disk3` refused).
Raw outputs: Mac `~/hwS2`.
FCE29EDC was never loaded: EJ was chosen instead (mode 0 default, eject spins down); S2's code
was merged with EJ, see "Batch EJ hardware results".

### Batch EJ hardware results

Rationale: the decision after S2 ("never spin down when idle, Eject spins the drive down",
2026-11-22); FreeBSD never stops a disk for idleness and has no eject of a fixed disk (design.md
"EJ"). Builds, all approved with a legacy-VNC step: 2CCCBE79 (detection by DKIOCEJECT's
open + SAM's "Power Off"), 5EFAFE09 (SAM's power state), 57C2CBC0 (spindown as ATA STANDBY
IMMEDIATE), 8A71718E (final: spindown = macOS's eject). Rollbacks ~/sas2308-rollback-<UUID8> of
each. 8 drives (ST18000NM000J, exFAT volumes /Volumes/<serial>), mode 0, raw outputs Mac `~/hwEJ`.

- What eject sends (95E0D6C9, debug log): unmount writes, a GPT re-read, SYNCHRONIZE CACHE, then
  START STOP UNIT START 0 in the same millisecond; DA "ejected disk, success" in that millisecond;
  SAM's state 4 -> 1; the IOMedia objects stay (`diskutil list` still shows the disk);
  `diskutil mount diskNs2` brings the volume back. SAM's PM state sampled every ~9 ms: 4 until
  0.76 s after the eject began (the eject returned at 0.26 s), then 1.
- 2CCCBE79: "Power Off" was set on all 8 SAM devices, yet the eject took 0.27 s and the stop was
  "media present" (the driver already closed): completed here, the drive kept spinning (Start_Stop
  442 -> 442, first read 0.021 s). Hence 515295d.
- (b) eject: drive7 and drive2 on 5EFAFE09, drive7 twice on 8A71718E: "Stops passed
  (ejected)" + 1 each, log `idle-type stop, SAM power state 4, no media reported (doEjectMedia)`
  and `START STOP UNIT (stop) sent: media ejected` (CDB `1b 00 00 00 00 00`); first 4K read 7.58 /
  7.52 / - / 7.51 s (spin-up; START 1 in the log); Start_Stop_Count 442 -> 443, 449 -> 450,
  443 -> 444, 444 -> 445; `diskutil mount` and a 64 MiB write + read back (after purge)
  identical; 192 unchanged.
- (b) `sas2308ctl spindown` on an unmounted drive: S2's START STOP UNIT (5EFAFE09) and the ATA STANDBY
  IMMEDIATE of 57C2CBC0 both left drive3 answering every READ NOT READY 04/02 (reads failed after
  3 s, mount failed; recovered by `diskutil eject` + `diskutil mount`). Now macOS's eject: drive3
  (57C2CBC0 kext, new tool) and drive8 (8A71718E): stop passed, first read 7.69 / 7.52 s,
  Start_Stop_Count + 1, mount + read/write fine.
- (a) + (c) `pmset -a disksleep 1`, 7 mounted + drive3 unmounted (not ejected), 240 s idle: all 8
  SAM states 4 -> 3 -> 2 -> 1; 8 idle stops completed here (log: `idle-type stop, SAM power state 3
  (idle timer)` x 8, debug level), none passed; first 4K reads 0.021-0.236 s (the unmounted drive
  0.031 s). Same on 5EFAFE09 (0.021-0.267 s).
- (d) sleep/wake with the 8 volumes mounted: Suspends 1, Resumes 1 (without reset), 8 drives, 8
  volumes, no ejected stop. Restart with the 8 volumes mounted: request -> boot 13 s (as S1), loaded
  8A71718E, 8 drives, config page failures 0, IO timeouts 0, the volumes mount. A shutdown_stall
  report as at every restart on this Mac since 2026-11-15 (before the kext; our hotplug thread idle
  in it). SMART 192 (Power-Off_Retract_Count) before / after all tests, drive6 drive2 drive7
  drive8 drive3 drive5 drive4 drive1: 49 59 54 52 54 54 56 61 / 49 59 54 52 54 54 56 61.
- (e) regression on 8 drives (unmounted): random 4K read QD32 60 s 1996 IOPS (249.5 per drive),
  seq 1 MiB read 60 s 2228.8 MB/s (278.6 per drive), verify 256 MiB per drive at 60-60.2 % 0
  mismatches, 0 errors; no timeouts or resets. `diskutil verifyVolume`: all 8 exFAT volumes OK;
  remounted at /Volumes/<serial>. End state: `pmset -a disksleep 10`, mode 0.

### Hardware tests still to run

Run on build DDA7E6AD or later, `sas2308ctl level debug` first, drives found with
`~/sasbench/sasdisks.sh`; after each test `sas2308ctl stats` and
`/usr/bin/log show --last 10m --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'`.

1. **D14 / M7, disk pull under IO.** Start `sasbench -b 4K -q 8 -p rand -t 300` on all 7 raw drives
   (unmounted). Pull one drive from its breakout. Expect: its sasbench job gets errors, the other 6
   continue at full rate; log `target N destroyed` and no `IOs still outstanding`; if the removal TM
   never answers, `IOs still outstanding 60000 ms after its removal: controller reinit` and the
   destroy only after it; no panic. Re-insert: the drive comes back (same disk = its old target id),
   `Config page failures` 0.
2. **D5, dock unplug under IO.** Mount one JHFS+ test volume, run `~/hwA/fsyncw <volume>/f 256 300`
   on it and `sasbench` reads on 4 raw drives, then unplug the Thunderbolt cable of the dock. Expect:
   no panic; log `terminating (controller gone)`, `surprise removal: controller disabled`, no
   `IOs still outstanding at termination`; the writers end with ENXIO/EIO; the volume is
   force-unmounted. Re-plug: the kext attaches again and finds the 7 drives.
3. **D8, disk swapped during sleep.** `pmset sleepnow`; while asleep, exchange two drives between
   their bays (or put a different drive in one bay); wake. Expect: drives that stayed keep their
   nubs (no `target N destroyed`); a bay with another drive: its old nub destroyed and a new one
   created, no IO of the old drive reaches the new one (`IDENTIFY shows a different disk` if that
   IDENTIFY failed at first); a mounted volume of a moved drive is not written through the wrong nub.
4. **D1, SATA SSD.** Attach a SATA SSD to a free breakout port; with IO on it, pull or reset another
   drive (any TM is freed). Expect: the SSD keeps its target (no `starting the queued removal`,
   no REMOVE_DEVICE for it); `MPS_TARGET_IS_SATA_SSD` is only used at shutdown (batch D, D9).
5. **D3, forced target reset.** The batch B trigger `sas2308ctl lose 1` (under reads on a raw
   drive) now drives a recovery to a target reset and a reinit unattended (done above, raw reads
   only). Still to do by hand: the same with a mounted JHFS+ volume under F_FULLFSYNC writes on that
   drive (`~/hwA/fsyncw`): no write error, `Unit attentions after reset retried` goes up.
6. **D10, OS task management from the family.** Nothing unattended makes SAM send AbortTask /
   LogicalUnitReset / TargetReset. If a tool or a stuck drive does: expect one `OS <type> on target
   N lun L tag T: <outcome>` log line per request (batch E, D23), a target or LUN reset as one TARGET_RESET (`TM target reset` +1, `TM requests from OS`
   +1), an abort as the task's recovery (`IO timeouts` +1), ABORT TASK SET / CLEAR TASK SET /
   CLEAR ACA rejected; never two TMs on one target at once.
7. **D11, dock unplug** (with test 2): the log must show `register 0x.. reads 0xffffffff: checking
   whether the controller is still there`, then `... PCI config space does not answer: controller
   gone` about 100 ms later; a link blip without an unplug shows `transient, ignored` and
   `Transient all-ones reads` +1 instead.
8. **D6 / M5c, breakout replug under IO** (batch C; decides removing the page 0 retries and the
   16 re-add slots). Build 6DD7CE42 or later. Start `sasbench -b 4K -q 32 -p rand -t 300` on all
   7 raw drives (unmounted). Pull one SFF-8087 breakout cable (its drives drop), wait 20 s, plug it
   back. Expect: its drives come back with their old target ids, the others keep full rate,
   `sas2308ctl stats`: "Config page failures" 0 and, the new question, "Config page retries" and
   "Device re-adds scheduled" 0 (with D6 a page read no longer waits for a shared buffer). If both
   stay 0 over 3 replugs, the retries (`mps_xnu_get_sas_device_pg0`) and re-add slots
   (`MPS_XNU_EVENT_READD` for adds; D8's re-IDENTIFY keeps the slot machinery) can go.
9. **D9, SATA SSD stopped at shutdown** (batch D, build EBB85578 or later). Attach a SATA SSD to a
   free breakout port (no data needed on it). Read its SMART "Unsafe Shutdown Count" (attribute
   192 / 174 / vendor's, e.g. `smartctl -A` from a Linux box, or with smartmontools on the Mac if
   it can reach a SCSI nub: `smartctl -d sat -A /dev/diskN`). Then 3 x `sudo shutdown -r now`
   (drives unmounted). Read the count again: it must not have grown. The INQUIRY at nub creation
   must have marked it (`sas2308ctl level debug` before attaching: log line `Target N supports
   SSU`); the shutdown itself is not in the log (logd stops first). A SATA HDD must get no SSU
   (enable_ssu default).
10. **D29, event acks on hot-plug: dropped.** Batch G1 removed the ack code (FreeBSD never acks;
    the rule needs no test to revert). With test 1 or 8 only check that the pull / re-insert still
    works without acks (nub destroyed and created, `sas2308ctl stats` Events grows).
11. **Batch E log levels on hot-plug** (with tests 1 and 2, default level info, i.e. without
    `level debug`; since batch G1 the default mask is FreeBSD's `info,fault`, the lines below are
    tagged `info|error`). Note the time, do the pull / re-insert and the dock unplug / re-plug, then
    `bash ~/hwE/count.sh --start "<time>"`. Expect: a pull shows FreeBSD's info lines for the
    removal and the re-add (no event field dumps, no `PHY[..]` lines); the dock unplug shows
    `controller gone (surprise removal)` and `surprise removal: controller disabled` as **Error**,
    no **Fault** line; a few dozen lines in all, not hundreds.

12. **Batch F, a lost command inside the halt at power off.** Needs someone to press the power
    button afterwards. Mount the JHFS+ test volume on disk9, write a file, then
    `sudo ~/sas2308-dext/tools/sas2308ctl lose 1; sudo shutdown -h now` (power off: SAM marks
    "Power Off" and spins the disks down with START STOP UNIT, timeout 0, during the halt). Expect
    the Mac to power off within ~35 s (unmount recovery) + <= 21 s (halt), no "Halt/Restart Timed
    Out" panic report after the next boot, the file verifies. Repeat without `lose` as the baseline.

13. **Batch G1, hot-plug on the hotplug taskqueue and the TM waiter** (with tests 1, 3 and 7, build
    of batch G1 or later). The pull and the re-insert must still destroy and create the nub (the
    hotplug body now runs on its own kernel thread, L10). Afterwards `sas2308ctl stats`:
    "TM waits in gate" (D43: 0 expected; non-zero means the family sends TMs inside our work-loop
    gate, the only case where the waiter still pumps the reply queue), "Lost completion kicks" 0,
    "KASSERT hits" 0.

14. **Batch G2, D50 at power off with the dock left on** (build 4746DE60 or later). Needs someone to
    press the power button. `sudo shutdown -h now` with the drives idle; leave the dock powered
    for at least 1 min; power the Mac on. Then `ioreg -r -c SAS2308HBA -l | grep "IOC state at
    attach"` (READY, OPERATIONAL or RESET: all fine; FAULT or all-ones: report),
    `sudo dmesg | grep -i dart` and `/usr/bin/log show --last boot --predicate 'sender == "kernel"' |
    grep -i dart` (no fault lines expected), all 7 drives present, `sas2308ctl stats` "Diag resets"
    0 or 1, "Config page failures" 0. The restart variant ran unattended (hardware results).

15. **Batch G2, D21 hot-add under a full task pool** (optional, with test 1's breakout). Start
    `sasbench -b 4K -q 320 -p rand -t 300` on 6 raw drives (6 x 320 = 1920 tasks plus the seventh
    drive's re-add; 2046 OS tasks) and pull and re-insert the seventh drive. Expect the nub to come
    back, "Device re-adds given up" 0, "Config page failures" 0.
16. ~~Batch S2, approve build FCE29EDC~~: superseded by EJ (8A71718E approved with a
    legacy-VNC step, 2026-11-22).
17. **Batch EJ, pull a drive after Eject** (build 8A71718E or later). With a volume mounted on one
    drive: `diskutil eject diskN` (or Finder's eject); `sas2308ctl stats` shows "Stops passed
    (ejected)" + 1; wait 20 s; pull the drive (or switch off its bay). Expect: no "disk not ejected
    properly" alert, log `target N destroyed`, the other drives unaffected; SMART 192 of that drive
    unchanged after it is back (`smartctl -A`). Plug it back: it comes back as a new disk and
    `diskutil mount diskNs2` mounts it. Repeat once with `sas2308ctl spindown diskN` on an
    unmounted drive instead of the eject.

## Ranked table: changes (CHANGE)

The 46 entries whose resolution includes a code change (D49(b) is listed below the table; D17's cosmetic compat change is included as row 18). Cat = preliminary verdict from the scan (U unnecessary, J justification missing/wrong, Q unclear). Risk H/M/L.

| # | ID | Cat | Risk | Ours | FreeBSD / reference | What differs | Agreed fix | Batch |
|---|---|---|---|---|---|---|---|---|
| 1 | D1 | U | H, bug | `core/mps_sas.h:55-56` | `mps_sas.h:55` | REMOVE_WAIT reuses SSD bit 6 | `1 << 7` + static assert (width, overlap) | A |
| 2 | D2 | U | H, bug | `core/mps_sas_lsi.c:506-512` | `mps_sas_lsi.c:774-805` | target-reset kick for a stuck IDENTIFY deleted | restore verbatim | A |
| 3 | D3 | U | H, bug | `kext/SAS2308Glue.cpp:428-437` | `scsi_da.c:2231-2245` | no RETRY_UA after AC_BUS_RESET/AC_SENT_BDR | mark tids, requeue one 29/xx UA per LUN uncounted (+HW) | A |
| 4 | D5 | J | M, bug | `kext/SAS2308HBA.cpp:503-519` | `mps_pci.c:340-343` | IO failed asynchronously after nubs are destroyed | synchronous disable + flush/drain before `super::willTerminate`; fix comment/design.md | A |
| 5 | D4 | Q | H | `kext/SAS2308Glue.cpp:260` | `mps_sas.c:1885` | hold time counts against the 30 s | HandleTimeout re-arms held / recently posted tasks (remaining >= 1 ms) (+HW) | B |
| 6 | D8 | Q | M | `core/mps_sas_lsi.c:419-436` | `mps_sas_lsi.c:715-728` | failed IDENTIFY = new disk, mounted volume dropped | keep tid/gen, hold IO until IDENTIFY decides (+HW) | A |
| 7 | D14 | Q | M | `kext/SAS2308Glue.cpp:526-535` | CAM periph refcount | destroy after 10 s with IO posted | wait for 0 or disabled; at the cap reinit, never complete a posted ccb | A |
| 8 | D6 | J | M | `core/mps_xnu.c:928-962` | `mps_config.c:148` | one lent DMA buffer for all config/IDENTIFY | compat maps caller buffers at load, released at free/next load; config/IDENTIFY verbatim | C |
| 9 | D10 | U | M | `core/mps_sas.c:2229-2390` | `mps_sas.c:998-1008` | six real OS TMs on a new path | target/LUN reset -> XPT_RESET_DEV; abort -> FreeBSD recovery; others rejected | B |
| 10 | D11 | Q | M | `core/mpsvar.h:509-521` | `mpsvar.h:499-502` | one all-ones read = gone for good | confirm 5x over ~100 ms in `mps_xnu_gone` | B |
| 11 | D12 | J | M | `core/mps.c:1008-1027` | `mps.c` attach (`mps_transition_ready`) | diag reset on every wake | FreeBSD attach sequence on resume, diag reset as fallback (+HW) | B |
| 12 | D13 | Q | M | `kext/SAS2308HBA.cpp:79-92` | `mps_pci.c:419-433` | only 2 command bits restored | `saveDeviceState`/`restoreDeviceState` | B |
| 13 | D15 | Q | M | `kext/mps_kext.cpp:478` | `mps_sas.c` action_scsiio callout | timeout 0 = no timer at all (SAM's SSU uses 0) | 10 min core backstop for 0 except FORMAT/SANITIZE; 30 s kept with corrected comment | B |
| 14 | D16 | Q | M | `core/compat/mps_compat.c:531-555` | `mps.c:953-964` | DELAY >= 1 ms drops mps_mtx | keep the lock (IODelay / IOSleep) | C |
| 15 | D18 | Q | M | `core/compat/mps_compat.c:266-277` | `mps.c:2727,2817` | panic returns, SGE overruns next frame | EFBIG at both sites | B |
| 16 | D7 | U | M/L | `core/mps_xnu.h:33` | `mps_sas.c:714` | 64 target ids | ids from `maxtargets`, arrays in the async interface, `use_phynum` back | D |
| 17 | D9 | Q | M | `core/mps_sas.c:1869,1990` | `mps_sas_lsi.c:1093-1220` | no SSU to SATA SSDs at shutdown | port FreeBSD's SSU (+HW with an SSD) | D |
| 18 | D17 | J | M | `core/compat/cam_compat.c:395-403` | `cam_xpt.c:5309-5320` | QFRZN released in xpt_done | keep + comment; cosmetic: map the TM's (tid,lun) freeze to the target in compat | C |
| 19 | D19 | U | L | `core/mps.c:906-937` | `mps.c:808-860` | PCI restore only after attempt 1 | after every diag reset | B |
| 20 | D20 | U/Q | L | `kext/mps_kext.cpp:99-137` | `cam_periph.c:1923-2031` | autosense fail retried; reset-killed R/W spends a SAM retry; no rescan on DEV_NOT_THERE | (a) CHECK CONDITION no sense; (b) requeue bounded; (c) rescan tid | A |
| 21 | D21 | U | L | `kext/mps_kext.cpp:330-337`, `core/mps.c:396-400` | `mps_sas.c:720-729` | 8-frame reserve computed in the core | core lines back to FreeBSD; reserve + reason in the glue | C |
| 22 | D31 | Q | L | `core/mps_config.c:38-155` | none | temp poll: 60 s block, then reinit | 5 s timeout, no reinit, stop after first timeout, resume after reinit | D |
| 23 | D32 | J | L | `core/mps.c:1843`, `kext/SAS2308HBA.cpp:360-371` | `mpsvar.h:48`, `mps.c:412-424` | two 1 MiB constants | `MPS_MAXIO_PAGES` back; 256 as tunable value; kext publishes `sc->maxio` | C |
| 24 | D51 | Q | L | `core/mps.c:2141-2160` | `mps_pci.c:340-343` | IO finished in the IOC failed at unload | one polled `mps_intr_locked` before the MUR; keep order, document | B |
| 25 | D43 | U | L | `kext/mps_kext.cpp:605-609`, `kext/SAS2308HBA_Props.cpp:482` | `mps.c` NO_SLEEP wait | pumps + 1 s delivery hide lost kicks | keep pumps (comment); 1 s timer counts lost kicks | B |
| 26 | D22 | U | L | `core/mps.c:1838` | `mps.c:1575` | default level adds event/mapping/init/recovery | default info,fault,error; XINFO lines DEBUG | E |
| 27 | D23 | U | L | `kext/SAS2308Glue.cpp:278-281` | `mps_sas.c:1553-1554` | duplicate kext lines | drop/lower as listed | E |
| 28 | D24 | Q | L | `kext/mps_xnu_os.cpp:97-122` | none | MPS_FAULT logged as ERROR | FAULT type (not for surprise removal) | E |
| 29 | D25 | U | L | `kext/mps_xnu_os.cpp:124-144` | console | device-less lines skip iolog | global iolog fallback, unit | E |
| 30 | D26 | Q | L | `core/mps_xnu.c:728-875` | `mps_sas.c:1904` | rate limiter hashed on handle % 8 | limiter in `struct mpssas_target` | E |
| 31 | D27 | U | L | `core/mpsvar.h:691-712` | kernel `%b` | 8 call sites patched for `%b` | `%b` in `mps_compat_vlog`, sites verbatim | C |
| 32 | D30 | Q | L | `core/mps.c:2542-2544` | `mps.c:2103-2120` | doorbell read per empty interrupt | remove; `mps_periodic` finds faults | D |
| 33 | D33 | U | L | `core/mps.c:1831-1852` | `mps.c:1570-1660` | no tunables | compat TUNABLE_*_FETCH; function verbatim | C |
| 34 | D35 | Q | L | `core/compat/cam_compat.c:404-416` | `cam_xpt.c:5309-5312` | held queue rerun per completion | stop at freeze, run once per pass | C |
| 35 | D36 | U | L | `kext/mps_kext.cpp:195-198` | none | dead AC_LOST_DEVICE branch | remove | C |
| 36 | D37 | U | L | `core/compat/cam_compat.h:110-134` | `cam_ccb.h:139-221` | XPT_*/AC_ADVINFO/priority values not CAM's | copy CAM's values | C |
| 37 | D39 | Q | L | `core/compat/mps_compat.c:688-793` | taskqueue kthread | taskqueue on a HIGH thread_call | kernel thread per queue, real drain | C |
| 38 | D40 | J | L | `core/compat/mps_compat.c:797-848` | per-hook intrhook | global hook list | run the caller's hooks only | C |
| 39 | D41 | Q | L | `core/compat/mps_compat.c:861-960` | busdma load | boundary not enforced | enforce/split in load_ccb (FINAL M5), inherit from parent | C |
| 40 | D42 | U | L | `core/compat/mps_compat.c:1029-1079` | `bus_dmamap_load` | error returned instead of callback-only | busdma contract | C |
| 41 | D44 | U | L | `kext/SAS2308HBA.cpp:245-249` | Apple `:1674-1691` | ProtocolTimeoutOccurred | TaskTimeoutOccurred | B |
| 42 | D45 | U | L | `kext/SAS2308Glue.cpp:226-244` | Apple `FindTaskForAddress` | own slot table, linear scans | family lookup, state in task data | D |
| 43 | D46 | U | L | `kext/mps_kext.cpp:487` | `scsi_da.c:1570-1578` | always SIMPLE | pass the attribute (no periodic ORDERED, commented) | D |
| 44 | D47 | U | L | `kext/SAS2308Glue.cpp:169-222` | `mps_sas.c:1850-1856` | zero-length rejected, mapping shortage fails | treat as NONE; requeue | D |
| 45 | D48 | U | L | `kext/SAS2308HBA.cpp:604` | `mps.c:1494-1500` | alignment 4 advertised | 1 | D |
| 46 | D52 | Q | L | `kext/Info.plist:37-44` | `mps_pci.c:123-137` | only 0x0087 | add 0x0086, 0x006E | D |

Small additions agreed inside KEEP entries (same batches): D49(b) resume failure logged FAULT, `fSuspended`
not set silently after a failed MUR (E/B); D54 compat `mps_reinit()` wrapper (C); D55 KASSERT-hits counter (D);
D65/D67/D69 comment + xpt_done assert (C); D73 no-bounce assert, D74 alignment to `withSpecification` and single
zeroing (D); D78 QUEUE_MACRO_DEBUG in host tests (C); D29 "Event acks sent" counter (D).

## Hardware tests

| ID | Test | After batch | Decides |
|---|---|---|---|
| D3 | F_FULLFSYNC after a forced target reset and after sleep/wake under IO; debug trace for 29/xx | A | confirms the bug and the fix |
| D8 | 5x sleep/wake, 7 spinning disks, mounted JHFS+ under writes | A (repeat after B) | no eject, 0 generation changes |
| D4 | timer re-arm once from HandleTimeout times out once | B | SCSIParallelTimer behaviour (source not available) |
| D12 | 5x sleep/wake logging which resume path ran, UA and re-IDENTIFY counts | B | READY path works on the 9207 |
| D13 | MSI address/data before/after a forced diag reset | B | defence vs live issue |
| D34 | `shutdown -r` while 7 drives write at full rate with write cache on; verify after boot; SYNCHRONIZE CACHE reached each drive before the MUR | B (run with C) | done: every block old or new data, 0 corrupt; trace not obtainable (logd stops first) |
| D29 | "Event acks sent" after boot + hot-plug | D | 0 -> remove acks (FreeBSD); >0 -> keep with the event codes in a comment |
| D9 | SMART unsafe-shutdown count of a SATA SSD over 3 `shutdown -r` | D (needs an SSD) | SSU port verified |
| D75 | BAR1 with kIOMapPostedWrite vs default: random 4K QD32 IOPS, 0 errors | D | mapping type |

## KEEP items that need a comment or doc change

| ID | Where | Text to add or correct |
|---|---|---|
| D5 | `kext/SAS2308HBA.cpp:503-519`, design.md "Termination" | the current "fails its IO at once" is wrong; describe the synchronous disable + drain (batch A) |
| D12 | `core/mps.c:1008-1027` | Linux `scsih_resume` uses SOFT_RESET first; ours: FreeBSD attach logic, diag reset fallback; 498 ms was the reinit only |
| D15 | `kext/mps_kext.cpp:478`, Info.plist note | 30 s = Linux sd default and the macOS SAS HBA value (FreeBSD da 60 s); 0 = family "no timeout", core backstop 10 min except FORMAT/SANITIZE |
| D17 | `kext/mps_kext.cpp:140` | QFRZN released here (no periph); retries reorder behind younger SIMPLE IO, which disks allow; TMs freeze the whole target |
| D21 | glue `mps_kext_max_tasks` | what the 8 internal commands cover |
| D28 | `core/mps_sas.c:1651` | EEDP not ported (no CAM advinfo); `eedp_enabled` set from IOCCapabilities but unused (the current comment says 0: wrong) |
| D38 | malloc hook | IOMalloc may block; legal: no core path at primary interrupt level (`FilterInterruptRequest` returns true without the core) |
| D43 | TM waiter / target drain | pumps: FreeBSD NO_SLEEP precedent; completions delivered in-gate from `HandleInterruptRequest` |
| D46 | `kext/mps_kext.cpp:487` | no periodic ORDERED tags: SATL has no ordered NCQ; measured worst latency 1.22 s vs 30 s |
| D49 | `kext/SAS2308HBA_PM.cpp` | synchronous setPowerState: measured drain + MUR + reinit < 1 s, 10 s bound |
| D50 | `kext/SAS2308HBA_PM.cpp` | MUR at shutdown: the dock's PSU keeps the IOC running across the Mac's restart; DART/IOVA invalid for the next boot |
| D51 | `kext/SAS2308HBA.cpp:438-439`, design.md | interrupt off before `mps_free`: the family owns the source, no `bus_teardown_intr` equivalent; one polled drain before the MUR |
| D65, D69 | `core/compat/cam_compat.h` | openings stored, not enforced (family throttles); boot hold no-op (HBA not a boot device) |
| D22 | design.md "Logging" | field dumps are MPS_EVENT-gated (not xinfo); new default |
