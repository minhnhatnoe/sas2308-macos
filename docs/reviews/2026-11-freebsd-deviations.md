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

Comment-only changes (no behaviour change) ride with whichever batch touches the file: listed in the last table.
Hardware-only items: D34 (full-rate shutdown + verify + SYNCHRONIZE CACHE trace) after batch B; D75
(kIOMapPostedWrite A/B) after batch D; D29 decided by batch D's counter.

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
| D34 | `shutdown -r` while 7 drives write at full rate with write cache on; verify after boot; SYNCHRONIZE CACHE reached each drive before the MUR | B | suspend/shutdown constants |
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
