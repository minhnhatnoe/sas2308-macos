# SAS2308 port: decisions Q&A (every deviation from FreeBSD mps / CAM)

Code reviewed: sas2308-dext **master 4596c06** (read-only; branch log-noise ignored). References: FreeBSD 8c9ce319fef7
`sys/dev/mps` (ref/freebsd-src), FreeBSD CAM from the same commit (`cam_xpt.c`, `cam_periph.c`, `scsi_da.c`, ...),
Apple IOSCSIParallelFamily / SAM / IOPCIFamily sources (local copy, `src/`). Line numbers: ours as on master 4596c06,
FreeBSD as in that tree.

**Skipped on purpose:** the check-condition completion log level and the "xfer 65536/0" TUR question (both on branch
log-noise); items FINAL.md lists as fixed; pure renames and formatting.

**Legend.** Prelim: U = no macOS need found, revert to FreeBSD; J = needed on macOS but the justification is missing
or wrong in code/design.md; Q = unclear, needs an answer or a hardware test; K = justified and documented, keep.
Risk H/M/L. "LIKELY BUG" = functional bug, not style.

**Format.** Each entry has an answer (`Answer:`) and a check of that answer (`Check:`), with further rounds where needed.
Each entry ends as `Status: RESOLVED-KEEP (reason)`, `RESOLVED-CHANGE (what)` or `OPEN-NEEDS-HW-TEST (which test)`.
Batch 1 (D1-D52) is the one to answer now. Batch 2 (D53-D81) is what the scan would keep; a one-line confirmation is enough.

**Likely bugs found (batch 1):**
- D1 `MPSSAS_TARGET_XNU_REMOVE_WAIT` = `MPS_TARGET_IS_SATA_SSD` (both `1 << 6`): a SATA SSD gets a target reset + REMOVE_DEVICE at the first freed TM. Verified.
- D2 a timed-out SATA IDENTIFY keeps the single config page buffer; FreeBSD's target-reset kick was deleted; all later page reads / adds / temperature reads fail until a reinit. Verified.
- D3 UNIT ATTENTION after our resets is passed to SAM; SYNCHRONIZE CACHE is sent once by SAM, so an fsync barrier after a reinit / wake / target reset can fail. Verified against Apple source; needs a hardware confirmation.
- D5 willTerminate on surprise removal: nubs destroyed while their IO is still in the core (the comment says the opposite). Verified.
- D4 (Q, risk H) hold time counts against the family timeout; IO posted late can trigger a TM cascade.

## Batch 1: deviations to answer now (U / J / Q), ranked by risk

### D1 MPSSAS_TARGET_XNU_REMOVE_WAIT has the same bit as MPS_TARGET_IS_SATA_SSD: SATA SSDs get removed
_(scan id A1)_
- Ours: `core/mps_sas.h:55-56`, `core/mps_sas_lsi.c:492-495`, `core/mps_sas.c:186` (`mpssas_free_tm` -> `mpssas_xnu_retry_recovery`), `core/mps_sas.c:2404-2419`, `core/mps_sas.c:330-353`
- Reference: `mps_sas.h:55` (`MPS_TARGET_IS_SATA_SSD (1 << 6)`), `mps_sas_lsi.c:752-755`; FreeBSD has no REMOVE_WAIT flag
- We do: we define `MPSSAS_TARGET_XNU_REMOVE_WAIT (1 << 6)`, which is the same bit as `MPS_TARGET_IS_SATA_SSD`. `mpssas_add_device` sets `targ->flags = MPS_TARGET_IS_SATA_SSD` for a SATA disk whose IDENTIFY word 217 is 1 (`mps_sas_lsi.c:612-613`). Every `mpssas_free_tm` calls `mpssas_xnu_retry_recovery`. That function sees bit 6 on a live SSD target with a non-zero handle, takes a TM, and calls `mpssas_xnu_send_remove`: TARGET_RESET with LINK_RESET, then `mpssas_remove_device` sends SAS_IO_UNIT_CONTROL REMOVE_DEVICE.
- Reference does: bit 6 only marks the SSD, for the shutdown START STOP UNIT policy (`mps_sas_lsi.c:1312,1323`).
- Documented: no (the flag is in design.md "Review fixes" H4; the collision is not mentioned)
- Question: why bit 6? It must be a free bit (1 << 7 or above). Also, why does no mock scenario report a SATA device with rotation rate 1, which would have caught this?
- Prelim: U; risk H; LIKELY BUG. The first TM freed after a SATA SSD is attached removes that SSD from the firmware: any IO recovery, any OS TM, or the hot-unplug of another disk. The OS is not told (no INREMOVAL, no rescan until `mpssas_remove_complete`).
- Lead check: verified. `core/mps_sas.h:55-56` both `1 << 6`; `core/mps_sas_lsi.c:493-494` sets `MPS_TARGET_IS_SATA_SSD` (from IDENTIFY rotation rate 1, `:613`); `mpssas_xnu_retry_recovery` (`core/mps_sas.c:2407-2419`, called from every `mpssas_free_tm`) sends the queued removal for any target with the bit. Fix: a free bit (1 << 7) and a host test with an SSD in the mock.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (bug, confirmed). `core/mps_sas.h:55-56` both `1 << 6`; `mps_sas_lsi.c:493-494` sets the SSD bit from IDENTIFY word 217; `mpssas_xnu_retry_recovery` (`mps_sas.c:2407-2419`) treats it as a queued removal on every `mpssas_free_tm`. No reason for bit 6: an oversight when H4 added the flag.
Fix: `MPSSAS_TARGET_XNU_REMOVE_WAIT (1 << 8)` (above every FreeBSD flag; also keeps room for FreeBSD's own future bits) + `_Static_assert` that it overlaps no MPS_TARGET_*/MPSSAS_TARGET_* flag. Host test: mock SATA disk with IDENTIFY rotation rate 1, then any TM freed (abort of another target) -> the SSD must not get TARGET_RESET/REMOVE_DEVICE. Also add an SSD to the default mock topology.

Check: CHALLENGE (fix as written is broken). `struct mpssas_target.flags` is `uint8_t` (`core/mps_sas.h:48`, same in FreeBSD): `1 << 8` truncates to 0 when stored, so REMOVE_WAIT would never be set and the H4 queued removal would silently stop working. Use the free bit `1 << 7` (FreeBSD uses 0-6), and make the `_Static_assert` also check `flag <= UINT8_MAX` (or `< 1 << (8 * sizeof(((struct mpssas_target *)0)->flags))`). Widening the field would change a FreeBSD struct for no gain. Rest of the fix and the host test: accepted.
Answer (round 3): accept. Verified `uint8_t flags` (`core/mps_sas.h:48`) and FreeBSD uses bits 0-6. Final fix: `MPSSAS_TARGET_XNU_REMOVE_WAIT (1 << 7)` + `_Static_assert` that it fits the field (`< 1 << (8 * sizeof(((struct mpssas_target *)0)->flags))`) and overlaps no other target flag; host test as agreed (SSD in the mock, TM freed -> no removal; queued removal still works).

Check (round 4): Fix verified: bit 7 is free in the `uint8_t` field (FreeBSD uses 0-6) and the static assert covers both width and overlap; tests cover the SSD and the queued-removal regression.

### D2 A timed-out SATA IDENTIFY holds the only config page buffer for good (FreeBSD's target-reset kick deleted)
_(scan id A2)_
- Ours: `core/mps_sas_lsi.c:506-512` (the loop is gone), `:696-708`, `:718-745`; `core/mps_xnu.c:928-951` (`cfg_busy`); `core/mps_sas.c:786-791`
- Reference: `mps_sas_lsi.c:774-805` (after an add, find the cm with `MPS_CM_FLAGS_SATA_ID_TIMEOUT` and send a TARGET_RESET so the IOC gives it back); `mps_sas_lsi.c:917` (a malloc buffer per IDENTIFY)
- We do: `mpssas_ata_id_timeout` leaves the cm INQUEUE, and it keeps `sc->xnu.cfg_page` busy until the IOC answers. Nothing makes the IOC answer: the target reset was deleted with no replacement. The 4 remaining tries of the IDENTIFY loop fail at once with ENOMEM, so `sas_address = 0` (see D8). After that, every `mps_config_get_sas_device_pg0`, IDENTIFY and IO Unit Page 7 read returns ENOMEM ("page buffer busy"). Re-adds are scheduled 8 times over about 30 s, then given up. This lasts until some reinit runs `mpssas_complete_all_commands`.
- Reference does: the stuck command keeps only its own malloc buffer. Later IDENTIFYs and page reads use fresh buffers. The target reset sent right after the add recovers the stuck command.
- Documented: code comment `mps_sas.c:786-791` covers only the reinit path. design.md does not mention it.
- Question: why was the SATA_ID_TIMEOUT -> TARGET_RESET loop of `mpssas_add_device` dropped? Restore it verbatim. Better, also remove the single-buffer design (D6), so that one stuck command cannot block discovery.
- Prelim: U; risk H; LIKELY BUG. FreeBSD's comment says this case happens "when a bad disk is in the topology". One bad SATA disk then stops every later hot-plug add, and stops temperature reads, until a reinit or reboot.
- Lead check: verified. FreeBSD `mps_sas_lsi.c:774-805` loop is absent after `mpssas_rescan_target` in ours (`core/mps_sas_lsi.c:509-515`); `mpssas_ata_id_complete` (`:718-728`) is the only release of the lent buffer for a timed-out IDENTIFY.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (bug, confirmed). The loop was dropped in 1b2aafd (M5a-1) with no recorded reason; `mpssas_ata_id_complete` (`mps_sas_lsi.c:718-728`) is the only release of the lent buffer for a timed-out IDENTIFY, and nothing makes the IOC return it.
Fix: restore FreeBSD `mps_sas_lsi.c:774-805` verbatim after `mpssas_rescan_target` (our `mpssas_alloc_tm`/`mpssas_send_reset` exist with the same contracts). With D6 fixed the buffer is per-command again, so a stuck IDENTIFY only costs its own cm. Host test: mock IOC that never answers one SATA PASSTHROUGH -> after the add a TARGET_RESET goes out, the late completion frees the buffer, a following page-0 read and a second hot-add succeed.

Check: Accepted: verbatim restore of FreeBSD `mps_sas_lsi.c:774-805`; the host test (stuck PASSTHROUGH -> TARGET_RESET -> late completion frees the buffer -> next add works) pins it.

### D3 The UNIT ATTENTION expected after a reset is not absorbed (CAM da DA_FLAG_RETRY_UA missing)
_(scan id M6)_
- Ours: `kext/SAS2308Glue.cpp:428-437` (AC_BUS_RESET / AC_SENT_BDR only logged), `kext/mps_kext.cpp:199-205`, `kext/SAS2308HBA.cpp:234-240` (CHECK CONDITION passed up unchanged); compat `core/compat/cam_compat.c:485-520` (async callbacks inline, no periph reaction)
- Reference: CAM `scsi/scsi_da.c:2231-2245` (AC_SENT_BDR/AC_BUS_RESET → `DA_FLAG_RETRY_UA`, pending ccbs get `DA_CCB_RETRY_UA`), `:4745-4746` (`daerror(..., SF_RETRY_UA)`); Linux scsi_error also retries UA. Apple `sam/IOSCSIBlockCommands/IOSCSIBlockCommandsDevice.cpp:403-445`: `SynchronizeCache` sends once and returns kIOReturnError on anything but GOOD; SAM absorbs 29/00 only in `ClearPowerOnReset` (`IOSCSIPrimaryCommandsDevice.cpp:767-775`), i.e. at power-on, not after our resets.
- We do: after a reinit, a resume (always a diag reset, D12) or a target reset, the first command per LUN gets CHECK CONDITION 29/xx and goes to SAM as is. READ/WRITE burns one of IOBlockStorageServices' 4 immediate retries (`IOBlockStorageServices.cpp:815-835`); SYNCHRONIZE CACHE, MODE SENSE, START STOP and pass-through fail.
- Reference does: the periph retries the predicted UA, uncounted, for every command.
- Documented: no. FINAL M1 named it; the M6 fix only re-sends commands killed by the reset itself.
- Question: why not port the periph rule into `mps_kext_io_done` (or cam_compat's done path): mark the target on AC_BUS_RESET/AC_SENT_BDR (per tid, generation-checked), re-issue once a ccb that completes CHECK CONDITION with sense key 6 / ASC 0x29 for a marked target (bounded like `xnu_resets`), then clear the mark?
- Lead check: verified the Apple SynchronizeCache path (single send, no UA handling) and the absence of any UA handling in our done path.
- Prelim: U (missing FreeBSD/CAM behaviour); risk H; LIKELY BUG: an F_FULLFSYNC (SYNCHRONIZE CACHE) right after any recovery or wake can fail to the filesystem. Needs a hardware confirmation (target reset / sleep under IO, then F_FULLFSYNC). Merges C2, B11.
- Status: RESOLVED (CHANGE + HW-TEST)

Answer: agree: CHANGE, then OPEN-NEEDS-HW-TEST to confirm the symptom. Confirmed in code: no UA handling anywhere in our done path (only the sense-name table `mps_xnu.c:283-287`); `Async` only logs AC_BUS_RESET/AC_SENT_BDR (`SAS2308Glue.cpp:428-437`); Apple `IOSCSIBlockCommandsDevice::SynchronizeCache` sends once and maps non-GOOD to kIOReturnError, and `IOBlockStorageServices::doSynchronizeCache` (`:423-439`) has no retry (only async R/W has `retriesLeft`, `:816-824`).
Fix (CAM da's rule in compat, core untouched): on AC_BUS_RESET / AC_SENT_BDR mark the affected tids (generation-checked); in the done path a ccb for a marked tid that completes CHECK CONDITION, sense key 6, ASC 0x29 is requeued once (bounded like `xnu_resets`, uncounted against SAM), and the mark is cleared on the first GOOD or after one absorbed UA per LUN. Host test: mock returns 29/00 to the first command per LUN after a target reset/reinit -> the OS sees GOOD for SYNCHRONIZE CACHE.
HW test: dd to a raw drive, `sas2308ctl` forced target reset (or sleep/wake) under IO, then F_FULLFSYNC on a JHFS+ volume of that drive; check the return code and the debug-level trace for 29/xx.

Check: Accepted: CAM da's RETRY_UA rule in compat/glue, host test for SYNCHRONIZE CACHE after 29/00, hardware F_FULLFSYNC test. Note da retries any sense key 6 while RETRY_UA is armed (`SF_RETRY_UA`), not only ASC 0x29; matching 0x29 only is fine but say so in the comment.

### D4 Held IO counts against the family timer; IO posted after a long hold gets the remainder only
_(scan id C1)_
- Ours: `kext/SAS2308Glue.cpp:260` (SetTimeoutForTask before `mps_kext_submit`), `kext/mps_kext.cpp:478`, `core/mps_sas.c:1386` (a held ccb at timeout → `CAM_CMD_TIMEOUT` → `ProtocolTimeoutOccurred`)
- Reference: FreeBSD `mps_sas.c:1885` (`callout_reset_sbt(&cm->cm_callout, ccb->ccb_h.timeout)` is armed when the command is posted to the IOC). CAM never times a ccb that is queued or frozen in a devq/simq.
- We do: the family timer starts in ProcessParallelTask. Time spent held in the thin CAM (TM recovery, reinit, discovery after resume) is part of the 30 s. A held task that expires is failed. A task posted at T−ε gets HandleTimeout ε later, and that starts the ABORT → LUN reset → target reset escalation on an IO that was never slow on the wire.
- Reference does: the timeout covers only the time the IOC owns the command. IO queued behind a recovery or reset never times out and is never aborted for it.
- Documented: design.md "IO arriving during recovery" (holds count against the timer). The escalation risk for IO posted late is not discussed.
- Question: why not follow FreeBSD? Either arm the family timer only when the ccb is posted (defer SetTimeoutForTask to the done/kick thread when a held ccb is sent), or have HandleTimeout re-arm (SetTimeoutForTask again) for a task that is held or was posted less than its timeout ago instead of starting a TM. Needs a check that SCSIParallelTimer::SetTimeout re-inserts an armed task; its source is not in src/.
- Prelim: Q; risk H. A slow reinit or TM chain can make innocent post-hold IO trigger further TMs (cascade), and SAM's 4 immediate retries can run out while the hold lasts.
- Status: RESOLVED (CHANGE + HW-TEST)

Answer: agree: CHANGE (match FreeBSD: the timeout covers only the IOC-owned time). Confirmed: SetTimeoutForTask is called before `mps_kext_submit` (`SAS2308Glue.cpp:260`), so hold time counts.
Fix: in HandleTimeout, if the ccb is still held, or was posted less than its timeout ago (record `posted_at` in `mps_kext_task` when the core posts it), re-arm with `SetTimeoutForTask(task, remaining)` instead of calling `mps_kext_timeout`. Apple `IOSCSIParallelInterfaceController.cpp:1619-1627`: SetTimeoutForTask just calls SCSIParallelTimer::SetTimeout; the timer removes an expired task before HandleTimeout, so re-arming from HandleTimeout is the same as the first arm (verify on hardware: a held task re-armed once must time out once). Host test with the fake family (`kext_glue_test`): hold 25 s then post -> no TM; post then 31 s silence -> abort.

Check: Accepted (re-arm from HandleTimeout; `TimeoutOccurred` loops `GetExpiredTask` then `Rearm`, `IOSCSIParallelInterfaceController.cpp:1635-1662`). Two conditions for the fix: `SetTimeoutForTask(task, 0)` means "the task's own duration" (our C1 fix relies on that), so clamp `remaining` to >= 1 ms, never 0; and the host test must show a re-armed task is not returned again by the same `GetExpiredTask` loop. SCSIParallelTimer source is not in src/, hence the HW check.

### D5 willTerminate: a surprise removal fails IO asynchronously, so nubs are destroyed with IO still outstanding
_(scan id C3)_
- Ours: `kext/SAS2308HBA.cpp:503-519` (comment: "the core fails its IO at once"), `kext/mps_kext.cpp:635-648`; `core/mps_xnu.c:524-533` (`mps_xnu_gone` only queues the reinit; `mps_xnu_disable` fails the IO later on the taskqueue)
- Reference: FreeBSD `mps_pci.c:340-343` (detach runs `mps_free`, which completes every ccb before the SIM and its periphs go; CAM periphs are reference-counted until their ccbs return). Apple `IOSCSIParallelInterfaceController.cpp:515-531` (willTerminate destroys targets 0..62 at once).
- We do: `check_present` sets GONE and queues the reinit. The `DeliverCompletions()` that follows finds nothing. `super::willTerminate` then destroys every nub while its posted or held IO is still in the core. The completions arrive later, from the reinit task or from `mps_free` in StopController, on destroyed nubs. The M7 drain (fail held IO, wait, deliver) runs only in the hotplug thread, and `QuiesceHotplug` has already stopped that thread.
- Reference does: all IO completes before the periph or device objects go away.
- Documented: the code comment says the opposite of what the core does. design.md "Termination" also says "fails its IO at once".
- Question: why not call `mps_xnu_disable` synchronously (or run the same flush and drain as M7 for every target) in willTerminate before `super::willTerminate`, and fix the comment?
- Prelim: J (the mechanism is needed, but its claim is wrong); risk M. LIKELY BUG on a Thunderbolt unplug under IO: a completion reaches the nub after DestroyTargetForID, the M7 scenario on the termination path.
- Lead check: verified. `willTerminate` (`kext/SAS2308HBA.cpp:503-519`) calls `DeliverCompletions` right after `mps_kext_check_present`, but `mps_xnu_gone` (`core/mps_xnu.c:524-533`) only queues the reinit, so nothing is completed yet when `super::willTerminate` destroys the nubs.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (bug, confirmed). `willTerminate` (`SAS2308HBA.cpp:503-519`) relies on `mps_kext_check_present` failing IO at once, but `mps_xnu_gone` (`mps_xnu.c:524-533`) only queues the reinit; `mps_xnu_disable` runs later on the taskqueue, after `super::willTerminate` has destroyed the nubs. The comment and design.md "Termination" are wrong.
Fix: when not present, in willTerminate (before super): drain the core taskqueue's pending reinit and call `mps_xnu_disable` synchronously under mps_mtx through a new kext entry (`mps_kext_disable_now`), then `DeliverCompletions()`, then run the M7 per-target flush+drain for every target. Fix the comment. Host test (`kext_glue_test`): posted + held IO on 3 targets, registers go all-ones, willTerminate -> every task completed (DEV_NOT_THERE) before the fake family's DestroyTargetForID; no completion after destroy.

Check: Accepted: synchronous disable + flush/drain before `super::willTerminate`, comment and design.md "Termination" fixed; the kext_glue_test (no completion after DestroyTargetForID) pins it. Waiting for the reinit task is bounded because every wait checks GONE.

### D6 Config pages and IDENTIFY use one lent `bus_dmamem` buffer instead of malloc
_(scan id A3)_
- Ours: `core/mps_xnu.c:928-962`, `core/mps_xnu.h:130-134,159`, `core/mps_config.c:121,150-153,250-258,294-297`, `core/mps_sas_lsi.c:649-657`; the M5c retry and re-add machinery `core/mps_xnu.c:965-1165`
- Reference: `mps_config.c:148,647,...` (`page = malloc(cm->cm_length, M_MPT2, M_ZERO|M_NOWAIT)` mapped by `bus_dmamap_load(buffer_dmat)`), `mps_sas_lsi.c:917`
- We do: one 1 KiB DMA buffer, allocated on first use and flagged busy. Each user lends it to its cm's map (`bus_dmamap_xnu_lend`). A second concurrent reader gets ENOMEM. That is why M5c needed the 3 in-line retries, the 16-slot re-add queue, and pseudo event 0xFF01.
- Reference does: one buffer per request, so readers never contend.
- Documented: design.md "SAS Device Page 0", "M5c"; code `mps_config.c:250-258` ("bus_dmamap_load maps bus_dmamem_alloc memory only")
- Question: why not make compat `bus_dmamap_load` map ordinary kernel memory? It could use an `IOMemoryDescriptor::withAddressRange` plus `IODMACommand`, or a small compat pool for `malloc(M_MPT2)` buffers loaded on `buffer_dmat`. Then `mps_config.c` and the IDENTIFY code would stay verbatim, and D2, the retries and the re-add slots would go away. The serialization through the event taskqueue (M5c `tq_active`) already prevents overlap, so the extra machinery mainly protects against our own single buffer.
- Prelim: J (the restriction comes from compat, not from macOS); risk M; root cause of D2 and of the M5c failures.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. The single buffer exists only because compat `bus_dmamap_load` can map nothing but `bus_dmamem` memory (`mps_config.c:250-258` comment); no macOS reason.
Fix (smallest step that removes the contention): compat keeps a small pool of DMA-able page buffers (e.g. 8 x 1 KiB from one dmamem allocation) and `malloc(..., M_MPT2, ...)` of <= 1 KiB for a cm that is then loaded on `buffer_dmat` takes one from the pool; `bus_dmamap_load` accepts pool memory. Then `mps_config.c` and the IDENTIFY code go back to FreeBSD's malloc/free verbatim, and `cfg_busy`, the in-line retries, the 16-slot re-add queue and pseudo event 0xFF01 can be removed in a second step after a hot-plug test (keep them until then). Pool exhaustion = ENOMEM as FreeBSD's M_NOWAIT. Host test: two concurrent page reads + a stuck IDENTIFY -> both reads succeed.

Check: CHALLENGE (mechanism). Compat `malloc` cannot know at allocation time that a block will later be DMA-loaded: every small `M_MPT2` allocation would take a pool slot, including `fw_event` and its `event_data` (`core/mps_sas_lsi.c:100,107,148,152`, up to one per firmware event; a topology burst exhausts 8 slots and events get dropped with ENOMEM). Two alternatives that keep `mps_config.c`/IDENTIFY verbatim: (1) FreeBSD busdma's own model: `bus_dmamap_load` of non-dmamem memory on `buffer_dmat` bounces into a pool page at load time, and `bus_dmamap_sync(POSTREAD)` copies back (the core already syncs the IO buffer map, FreeBSD `mps.c:3018,3233`; our sync is a no-op today, D73, so it must become a copy for bounced maps); or (2) map the caller's buffer at load (`IOMemoryDescriptor::withAddressRange` + `IODMACommand` on the mapper, prepare/complete per load). Pool exhaustion then only affects concurrent DMA loads, as FreeBSD's bounce pages do. Which one?
Answer (round 3): accept option (2). Final fix: compat `bus_dmamap_load` of non-dmamem memory on `buffer_dmat` maps the caller's buffer at load (`IOMemoryDescriptor::withAddressRange` + `IODMACommand` on the device mapper, prepare at load / complete at unload); no pool, no copy, so `bus_dmamap_sync` stays a no-op (coherent DART, D73) and `fw_event` mallocs are untouched. Only config/IDENTIFY loads take this path (rare, may sleep: legal under lck_mtx). Then `mps_config.c`/IDENTIFY go back to malloc/free verbatim; cfg_busy/retries/re-add slots removed after a hot-plug test.

Check (round 4): Accepted option (2), with one required detail: FreeBSD's synchronous config paths and `mps_sas_lsi.c` never call `bus_dmamap_unload` (only `mps_config_complete` does, FreeBSD `mps.c:3235`), so once `mps_config.c`/IDENTIFY are verbatim again the per-load IOMemoryDescriptor + prepared IODMACommand would leak on every config read (one per 30 s poll and per add) and leave a DART mapping on freed memory. Compat must release a caller-buffer mapping no later than (a) `free()` of that block (the compat malloc header can record the live map) and (b) the next load of the same map. Host test: 1000 page reads -> live mapping count returns to 0; free of a mapped block unloads it.

### D7 Target ids capped at 64 (FreeBSD: IOCFacts MaxTargets, 1024 on the 9207-8e)
_(scan id M1)_
- Ours: `core/mps_xnu.h:33` (`MPS_XNU_MAX_TARGETS 64`), `core/mps_xnu.c:451-484` (`mps_xnu_get_tid` returns `MPS_MAP_BAD_ID` for the 65th), `core/mps_sas_lsi.c:439-451`; `kext/mps_xnu_os.h:39,55-61` (`MPSK_MAX_TARGETS`, async as 64-bit tid/present/gen masks), `kext/mps_kext.cpp:183-213`, `kext/SAS2308HBA.h:71`, `kext/SAS2308HBA_Targets.h` (`uint64_t` masks), `kext/SAS2308HBA.cpp:539-540`
- Reference: `mps_sas.c:714` (`maxtargets = MaxTargets + MaxVolumes`), `:926` (`max_target = maxtargets - 1`), `mps_sas_lsi.c:687-711` (mapping, then the `use_phynum` PhyNum fallback); Apple: `ReportHighestSupportedDeviceID` takes any value
- We do: ids 0..63 only. The 65th end device (SEPs and expanders' virtual devices count) logs "Could not get ID" and never gets a nub, although the core sizes `targets[]` for MaxTargets. The `use_phynum` fallback is gone.
- Reference does: ids up to MaxTargets-1 from the persistent mapping, PhyNum as fallback.
- Documented: only "target IDs 0..63" in the kext header; design.md "Termination" mentions ID 63; no reason for 64.
- Question: is 64 anything but the width of a `uint64_t`? The -8e is an external HBA; one 60-bay JBOD plus its SEP, or two 36-bay shelves, exceed it. Why not size the id space from `sassc->maxtargets` and pass bitmap arrays (or a tid/present/gen list) through the os interface?
- Prelim: U (no macOS reason found); risk M with an expander/JBOD, L on the current 8-PHY setup. Merges area findings A4, B4, C10.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (low priority). 64 is only the width of the `uint64_t` masks in the async interface; no macOS limit (`ReportHighestSupportedDeviceID` takes any value). The current card has 8 PHYs and no expander, so it can't bite today; an expander/JBOD would.
Fix: size the id space from `sassc->maxtargets` (MaxTargets+MaxVolumes = 1024 here), pass tid/present/gen as arrays (or bitmaps of `howmany(maxtargets, 64)` words) through `mps_kext_host.async`, restore the `use_phynum` fallback from FreeBSD `mps_sas_lsi.c:687-711`. Host test: mock topology with 70 end devices -> 70 nubs.

Check: Accepted: id space from `sassc->maxtargets`, arrays through the async interface, `use_phynum` fallback restored; 70-device mock test.

### D8 A failed SATA IDENTIFY makes the disk "unknown": new generation, the OS drops a mounted disk
_(scan id A5)_
- Ours: `core/mps_sas_lsi.c:419-436` (`sas_address = 0`), `core/mps_xnu.c:446-497` (ident 0: the id of this handle, `tid_gen++`)
- Reference: `mps_sas_lsi.c:715-728` (on failure FreeBSD keeps the IOC-made SAS address from page 0 and maps on it)
- We do: if all 5 IDENTIFY attempts fail during an add, the identity is 0. The id is reused but its generation changes, so the kext destroys the old nub and creates a new one. This happens also during the rediscovery after every reinit (`mpssas_handle_reinit` zeroes handles, and every disk is added again).
- Reference does: the device keeps its mapped id. CAM sees the same target.
- Documented: code comment `mps_sas_lsi.c:426-430`; design.md "Review fixes" C2 ("unknown = new disk")
- Question: during a reinit's rediscovery, why not fall back to "same handle and same IOC SAS address as before the reset = same disk" instead of "unknown = new"? As it is, one slow or timed-out IDENTIFY (spin-up after a diag reset; D2 makes it more likely) removes a mounted volume. A swap during a reset is far less likely than a slow IDENTIFY.
- Prelim: Q; risk M; needs a hardware check: diag reset with spinning SATA disks under IO.
- Status: RESOLVED (CHANGE + HW-TEST)

Answer: agree: CHANGE, then OPEN-NEEDS-HW-TEST. During a reinit's rediscovery an IDENTIFY failure should not mean "new disk".
Fix: if IDENTIFY fails and this handle's previous target had the same IOC SAS address (page 0) and the same PHY, keep the previous identity (same disk, generation unchanged) and schedule one re-IDENTIFY later; only a successful IDENTIFY with a different serial/WWN means a swap. Outside a reinit keep FreeBSD's behaviour (map on the IOC SAS address). Host test: reinit where one IDENTIFY times out -> same tid, same generation, no destroy/create.
HW test: sleep/wake (always a diag reset) with 7 spinning disks and a mounted JHFS+ volume under write load, repeated 5x -> no volume ejected, 0 generation changes in the log.

Check: CHALLENGE (data safety). For a direct-attached SATA disk the IOC's made-up SAS address is derived from the HBA address + PHY, so "same IOC SAS address and same PHY" identifies the bay, not the disk: exactly the C2 case. Keeping the old identity until a later re-IDENTIFY lets a disk swapped during sleep receive the old volume's writes in that window (FINAL C2's corruption). Proposal: keep tid and generation (no destroy/create), but hold the target's IO (devq freeze, as `mpssas_prepare_for_tm` does) until IDENTIFY succeeds; same serial/WWN -> release, different -> destroy + create; still failing after the re-add schedule (8 tries) -> fail the held IO DEV_NOT_THERE and treat as new. Host test: reinit with one IDENTIFY timing out, IO to that target held, released after the retry succeeds; swap variant -> no IO reaches the new disk under the old nub.
Answer (round 3): accept. Correct: same IOC SAS address + PHY is the bay (exactly C2). Final fix: keep tid and generation, freeze the target's devq (as `mpssas_prepare_for_tm`) until IDENTIFY succeeds; same serial/WWN -> release; different -> fail held IO DEV_NOT_THERE, destroy + create; still failing after the 8-try re-add schedule -> fail held IO, treat as new. Host tests as you list (retry succeeds -> IO released; swap -> no IO reaches the new disk under the old nub).

Check (round 4): Fix verified: identity kept, IO held behind a devq freeze until IDENTIFY decides; swap and give-up paths fail held IO before destroy; host tests plus the 5x sleep/wake HW test.

### D9 No START STOP UNIT to SATA SSDs at shutdown (supports_SSU, mpssas_ir_shutdown removed)
_(scan id A6)_
- Ours: `core/mps_sas.c:1869,1990` (XNU comments), `core/mps.c:1844` (`enable_ssu` still set, never read), `core/mps.c:2005`
- Reference: `mps_sas.c:2250-2261` (INQUIRY of a SATA direct-access end device sets `supports_SSU`), `mps_sas_lsi.c:1093-1220` (`mpssas_SSU_to_SATA_devices`), `:1225` (`mpssas_ir_shutdown` from `shutdown_final`). The default `MPS_SSU_ENABLE_SSD_DISABLE_HDD` (`mps.c:1581`) stops SSDs only.
- We do: nothing at shutdown. The H5 path drains for 1 s, sends a MUR and fails new IO.
- Reference does: sends SSU (stop) to every SATA SSD before power-off. FreeBSD added this because SATA SSDs count an unsafe shutdown and can lose their volatile cache.
- Documented: design.md M5a-3 "Not ported: ... SATA identify/START STOP UNIT"; no reason given
- Question: does macOS itself stop these disks at shutdown? The old SAM source sends `START_STOP_UNIT(…,1,0,0,0,0)` on the SystemSleep transition (`src/sam/IOSCSIBlockCommands/IOSCSIBlockCommandsDevicePM.cpp:409-436`). Whether the halt path reaches that before our `systemWillShutdown` fails IO is unverified. If it does not, the FreeBSD code can be ported almost verbatim. It needs only the INQUIRY snoop, which works for `CAM_DATA_SG_PADDR` if done from the sense or reply rather than from `data_ptr`.
- Prelim: Q; risk M; hardware test: SMART "Unsafe Shutdown Count" of a SATA SSD across 3 `shutdown -r`.
- Also (scan id C21 = D50): the shutdown path stops the IOC (MUR) but sends no SSU; FreeBSD's default `enable_ssu` = SSD only (`mps.c:1581`). Porting `mpssas_SSU_to_SATA_devices` would keep FreeBSD's default.
- Status: RESOLVED (CHANGE + HW-TEST)

Answer: agree: CHANGE (port FreeBSD's SSU verbatim, default SSD-only = no effect on today's HDDs), and OPEN-NEEDS-HW-TEST for the SSD case. Not ported only to save time in M5a; no macOS reason. Whether SAM's SystemSleep-path START STOP reaches the disks before our `systemWillShutdown` fails IO is unverified; FreeBSD's own SSU is the safe path either way.
Fix: restore the INQUIRY snoop that sets `supports_SSU` (from the completion's sense/reply path, not `data_ptr`, since we use CAM_DATA_SG_PADDR), `mpssas_SSU_to_SATA_devices` and the call from our shutdown path before the MUR. HW test (needs a SATA SSD on the card, none attached now): SMART Unsafe Shutdown Count across 3 `shutdown -r`.

Check: Accepted: FreeBSD's SSU (INQUIRY snoop from the reply path, `mpssas_SSU_to_SATA_devices`) before the MUR; SSD-only default; SMART test when an SSD is attached.

### D10 OS task management: six real TMs on a new XPT_XNU_TM path (FreeBSD fakes aborts, only XPT_RESET_DEV reaches the IOC)
_(scan id M4)_
- Ours: core `core/mps_sas.c:2229-2390` (`mpssas_action_xnu_tm`, `mpssas_xnu_tm_complete`), `:736`, `:884-897` (`mpssas_tm_timeout` completes the OS ccb); compat `core/compat/cam_compat.c:287-296` (XPT_XNU_TM bypasses frozen sim/devq); kext `kext/SAS2308HBA.cpp:739-822`, `kext/mps_kext.cpp:562-633` (caller blocks up to 30 s, pumps the reply queue every 10 ms)
- Reference: `mps_sas.c:998-1008` (XPT_RESET_DEV → `mpssas_action_resetdev` = target reset; XPT_ABORT, XPT_RESET_BUS, XPT_TERM_IO → "faking success" CAM_REQ_CMP), `:3031`; CAM `cam_xpt.c:2586-2598` (XPT_RESET_DEV is device-queued, waits behind a frozen devq); Apple: the requests come only through `IOSCSIProtocolServices.cpp:1526-1591` → `IOSCSIParallelInterfaceDevice.cpp:1749-1810` (user client / path manager; rare)
- We do: ABORT_TASK, ABORT_TASK_SET, CLEAR_TASK_SET, CLEAR_ACA, LUN reset, target reset all go to the IOC with their own TM and wildcard freeze, concurrently with a FreeBSD recovery TM on the same target (`targ->tm` not checked; FINAL L6 still open), and they bypass frozen queues.
- Reference does: OS aborts never reach the IOC; the only OS reset is a target reset through FreeBSD's own function; timeouts escalate abort → LUN reset → target reset with one TM per target.
- Documented: design.md M5a-3 "OS interface" describes it, says not why; FINAL L6 open.
- Question: why not map TargetReset (and LogicalUnitReset) to FreeBSD's `XPT_RESET_DEV`, AbortTask on a posted ccb to the FreeBSD timeout recovery (`mps_xnu_ccb_timeout`, serialized via `targ->tm`) or to FreeBSD's fake success, and return FUNCTION_REJECTED for ABORT TASK SET / CLEAR TASK SET / CLEAR ACA? That deletes ~250 lines of new core+kext code and the L6 race. Does anything on macOS ever send the last three?
- Prelim: U; risk M. Merges A7, C6, B10.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. The six-TM path was written to answer every family request literally; nothing measured says macOS needs it (the requests come from SAM's abort/reset paths, rare), and it adds L6 (two TMs per target).
Fix: TargetReset and LogicalUnitReset -> FreeBSD's XPT_RESET_DEV (`mpssas_action_resetdev`, a target reset; FreeBSD has no OS LUN reset), AbortTask on a posted ccb -> FreeBSD's timeout recovery for that ccb (`mps_xnu_ccb_timeout`, serialized via `targ->tm`), AbortTask on a held ccb -> complete it aborted, ABORT TASK SET / CLEAR TASK SET / CLEAR ACA -> FUNCTION_REJECTED. Removes ~250 lines incl. XPT_XNU_TM. Host test: OS abort while a recovery TM is active on the same target -> one TM at a time; HW check with `sas2308ctl` that the family path still completes.

Check: Accepted: FreeBSD's XPT_RESET_DEV for target/LUN reset, FreeBSD recovery for aborts of posted IO, held IO completed aborted, the other three rejected; host test for one TM per target. Also delete `XPT_XNU_TM` from compat (the B10 bypass in `cam_compat.c:287-296` goes with it).

### D11 Any all-ones register read marks the controller gone for good (outside the 2 s resume window)
_(scan id A8)_
- Ours: `core/mpsvar.h:509-521`, `core/mps_xnu.c:524-560`, `MPS_XNU_ABANDON` `core/mpsvar.h:300-312`; resume tolerance `core/mps.c:1152-1167`
- Reference: `mpsvar.h:499-502` (plain read)
- We do: the first 0xFFFFFFFF from any register sets GONE and requests a reinit. The reinit calls `mps_xnu_disable`: bus mastering off, every ccb failed, wildcard rescan. Only a reload recovers.
- Reference does: no check. A dead controller is found by the periodic FAULT check or by command timeouts.
- Documented: design.md "Rules for the port" ("all-ones register reads = surprise removal"); FINAL M8 was fixed for resume only
- Question: what if one read returns all-ones without an unplug: during the diag reset window (HOST_DIAGNOSTIC polled 256 ms after RESET_ADAPTER, FreeBSD `mps.c:202-230`), or on a Thunderbolt link retrain? Why not confirm before declaring the controller gone, by re-reading the doorbell and the PCI vendor id a few times over about 100 ms, as resume already does? The confirmation could go in compat or in `mps_xnu_gone`, keeping `mps_regread` close to FreeBSD.
- Prelim: Q; risk M; one bad read turns a recoverable event into permanent loss of every disk.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. One all-ones read declaring the controller gone for good is too strong; resume already confirms (`mps.c:1152-1167`).
Fix (in `mps_xnu_gone`, `mps_regread` stays FreeBSD's): don't set GONE inline; queue a confirm step on the taskqueue that re-reads the doorbell and the PCI vendor id (config space) 5 times over ~100 ms; GONE only if all fail, else count a "transient all-ones read" statistic and continue (a diag reset in progress is already excluded by INDIAGRESET). Host test: one all-ones read -> no disable, counter 1; persistent all-ones -> disable.

Check: Accepted: confirm-before-GONE in `mps_xnu_gone` with a counter; host tests for transient and persistent all-ones.

### D12 Resume always runs a full diag reset (mps_reinit); Linux, which the comment cites, does a soft reset
_(scan id A9)_
- Ours: `core/mps.c:1008-1027` (comment), `:1138-1171` (`mps_xnu_resume` -> `mps_xnu_request_reinit`), `:906-907` (`mps_reinit` always starts with `mps_diag_reset`)
- Reference: FreeBSD `mps_pci.c:389-399` suspend/resume return EINVAL (no PM); Linux `src/scsih.c:13714-13735` `scsih_resume` -> `mpt3sas_base_hard_reset_handler(ioc, SOFT_RESET)`, which tries MUR to READY first and falls back to a diag reset
- We do: every wake reboots the IOC firmware: diag reset, IOCFacts, IOCInit, port enable, full rediscovery. Every disk is re-added, so the generation and identity logic runs (D8).
- Reference does: N/A on FreeBSD. Linux uses MUR when the IOC kept power.
- Documented: code comment says "scsih_resume: hard reset", which is inaccurate (the reset type is SOFT_RESET)
- Question: why not check the doorbell on resume and, when it is READY or RESET (not FAULT), go straight to `mps_transition_ready` plus IOCInit? Was the wake time with a diag reset measured on the 9207 (firmware boot, link-up of 7 to 24 disks)?
- Prelim: J (the justification is wrong); risk M; long wakes and a full rediscovery on every wake.
- Status: RESOLVED (CHANGE + HW-TEST)

Answer: KEEP the diag reset, CHANGE the comment. Measured: wake under IO reinitialised in 498 ms with 7 drives, same target ids (measured on the test setup), so the cost the question worries about is small on the 9207. Reason to keep: across Mac sleep the Thunderbolt tunnel goes down and the dock's PCIe link is retrained; whether the IOC kept its state is not knowable cheaply, and the diag reset handles both cases with one tested path. Comment fix: "follows Linux mpt3sas in shape (block, stop the IOC, reset on resume); Linux tries a soft reset (MUR) first, we always diag-reset: measured 0.5 s wake, one recovery path". D8 (identity across the rediscovery) is the real risk and is fixed there.

Check: CHALLENGE. "Whether the IOC kept its state is not knowable cheaply" is not right: it is one doorbell read, and FreeBSD already has the code that acts on it: `mps_transition_ready` (attach path) leaves a READY IOC alone, MURs an OPERATIONAL one and diag-resets only on FAULT or an unknown state. Our suspend MURs the IOC to READY; if the dock kept power it is still READY at wake. Using FreeBSD's attach sequence (transition_ready -> IOCFacts -> IOCInit -> port enable) on resume is closer to FreeBSD than always calling `mps_reinit`'s diag reset, and it avoids a PHY/link reset of every drive on every wake (fewer UAs, D3; no forced re-IDENTIFY of SATA disks, D8). The 498 ms in measured on the test setup is the reinit time, not the cost of the UA/IDENTIFY consequences, and it was measured before D3 existed. Keep the diag reset as the fallback when the doorbell is FAULT/unknown or the MUR path fails. If you keep always-diag-reset anyway, D3 and D8 become prerequisites and the comment must say so.
Answer (round 3): accept. Verified our suspend MURs the IOC to READY, and `mps_transition_ready` already handles READY / OPERATIONAL (MUR) / FAULT-unknown (diag reset). Final fix: resume runs FreeBSD's attach sequence (transition_ready -> IOCFacts -> IOCInit -> port enable -> rediscovery) with `mps_reinit`'s diag reset as the fallback when that fails; comment says the 498 ms figure was the reinit only. HW test: 5 sleep/wake cycles under IO, log which path ran, count UAs (D3) and re-IDENTIFYs.

Check (round 4): Fix verified: `mps_transition_ready` (READY: nothing, OPERATIONAL: MUR, FAULT/unknown: diag reset) is FreeBSD's attach logic, diag-reset fallback kept; HW test logs which path ran and counts UAs/re-IDENTIFYs.

### D13 mps_pci_restore after a diag reset re-enables only memory decode and bus mastering
_(scan id M2)_
- Ours: `kext/SAS2308HBA.cpp:79-92` (`HostPciRestore`: `setMemoryEnable` + `setBusLeadEnable`), via `kext/mps_xnu_os.cpp:563-567`; caller `core/mps.c:914`
- Reference: `mps_pci.c:419-433` (`pci_cfg_restore`: BARs, command, MSI address/data/enable); Apple `pci/IOKit/pci/IOPCIDevice.h:544-545` (`saveDeviceState` / `restoreDeviceState` are public)
- We do: two command-register bits.
- Reference does: restores the whole saved config space.
- Documented: design.md "Stubbed / deferred" ("whether the SAS2308 diag reset clears more of its config space is to be checked on hardware"); comment in SAS2308HBA.cpp.
- Question: why not `saveDeviceState()` at attach and `restoreDeviceState()` here, which is FreeBSD's behaviour? If RESET_ADAPTER clears the MSI capability, every reinit and every wake (D12: always a diag reset) leaves the HBA without interrupts, with only the 1 s `mps_periodic` and the polling paths moving the reply queue. (Linux mpt3sas does no restore, so it is probably harmless; a hardware read of the MSI address/data after a forced diag reset settles it.)
- Prelim: Q; risk M (resume path). Merges B5, C11. See also A10 (restore only after the first of 3 attempts).
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (FreeBSD's behaviour, cheap). Hardware evidence says MSI survives the diag reset today (after the wake test IO completed at full rate, not at the 1 s periodic pace), so this is defence, not a live bug.
Fix: `fPCI->saveDeviceState()` once after attach, `restoreDeviceState()` in `HostPciRestore` (both public in IOPCIDevice.h:544-545), keeping memory-enable/bus-lead as a fallback. HW check: read MSI address/data before and after a forced diag reset (`ioreg`/`lspci`-equivalent via `IOPCIDevice` config reads in `sas2308ctl`).

Check: Accepted: `saveDeviceState`/`restoreDeviceState` (public, `pci/IOKit/pci/IOPCIDevice.h:544-545`) with the current two bits as fallback; MSI address/data HW check.

### D14 Hotplug destroy waits at most 10 s, shorter than the removal TM's own 30 s timeout
_(scan id C4)_
- Ours: `kext/SAS2308Glue.cpp:526-535`, `kext/SAS2308Glue.h:77` (`kDrainMs = 10000`), `kext/mps_kext.cpp:712-732`; `core/mps_sas.c:1084` (`MPS_RESET_TIMEOUT 30`)
- Reference: CAM periph lifetime is reference-counted: `cam_periph_invalidate` → the periph is freed when its last ccb returns, with no time limit. FreeBSD `mpssas_prepare_remove` → target reset → REMOVE_DEVICE.
- We do: since H4, the rescan (and so the destroy work) starts before the removal's target reset. If that TM needs more than 10 s (an unresponsive device: up to 30 s, then a reinit), the nub is destroyed with posted IO. The log says "N IOs still outstanding at its destroy" and the M7 use-after-destroy is back.
- Reference does: waits as long as needed. The TM timeout and the reinit bound it.
- Documented: design.md Review fixes M7 ("waits ≤ 10 s"), with no reason given for 10 s.
- Question: why 10 s and not "until outstanding == 0 or the controller is disabled", which is bounded by MPS_RESET_TIMEOUT plus the reinit? Or why not complete the remaining ccbs as DEV_NOT_THERE before destroying (they would come back later as stale replies, which the core already ignores)?
- Prelim: Q; risk M.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. 10 s was picked as "longer than a normal drain", before H4 moved the rescan ahead of the removal TM; it is now shorter than the TM's own 30 s (`MPS_RESET_TIMEOUT`).
Fix: wait until the target's outstanding count is 0 or the controller is disabled, with a hard cap of MPS_RESET_TIMEOUT + the reinit bound (60 s) only as a last resort, and at the cap complete the leftovers DEV_NOT_THERE before destroying (stale replies are already dropped by D56). Host test: removal TM that answers after 25 s -> nub destroyed only after its IO completed.

Check: CHALLENGE (DMA safety). "At the cap complete the leftovers DEV_NOT_THERE" returns buffers to the OS while the IOC still owns the commands and may still DMA into them (design.md M4 rule: no buffer back while the IOC could DMA; stale-reply dropping, D56, does not stop the DMA). At the cap the only safe ways out are the ones the core already has: request a reinit (diag reset ends IOC ownership, then `mpssas_complete_all_commands`) and wait for it, or keep the nub until the commands come back. Waiting until outstanding == 0 or disabled: accepted. Host test must also check that no ccb of a posted command is completed before the IOC returned it or a reset ran.
Answer (round 3): accept. Right: completing at the cap returns buffers the IOC may still DMA into (M4 rule). Final fix: wait until outstanding == 0 or the controller is disabled; at the cap request a reinit and keep waiting for it (diag reset ends IOC ownership, then `mpssas_complete_all_commands`), never complete a posted ccb ourselves. Host test also asserts no ccb of a posted command completes before the IOC returned it or a reset ran.

Check (round 4): Fix verified: never completes a posted ccb itself; the cap escalates to a reinit and waits; the host test asserts the DMA-ownership rule.

### D15 Timeout values: 30 s Read/Write Time Out (da: 60 s); timeout 0 disables both the family timer and the core backstop
_(scan id M7)_
- Ours: `kext/Info.plist:56-59` (30000/30000), `kext/SAS2308HBA_Hotplug.cpp:179-182` (copied to each nub), `kext/mps_kext.cpp:478` (`timeout_ms ? timeout_ms + MPSK_BACKSTOP_MS : 0`), `core/mps_sas.c:1675-1676` (timeout 0 = no core callout), `kext/mps_xnu_os.h:160-168`
- Reference: CAM `scsi/scsi_da.c:1559` (`DA_DEFAULT_TIMEOUT 60`); `mps_sas.c` action_scsiio arms `callout_reset_sbt(SBT_1MS * ccb_h.timeout)` unconditionally (every periph sets a finite timeout); Apple `IOSCSIPrimaryCommandsDevice.cpp:304-306` (no key = 0 = infinite)
- We do: R/W tasks get 30 s from submit, including hold time (D4), with up to 2040 commands in the IOC and NCQ 32 on SATA. A task with GetTimeoutDuration 0 (which ones? FORMAT UNIT, SANITIZE, user pass-through) has no timer at all.
- Reference does: 60 s from post for R/W; every dispatched ccb is timed.
- Documented: design.md "Properties published" ("as AppleLSIFusionSAS"); FINAL C1 fix (backstop = task timeout + 10 s).
- Question: why not 60000 (da), given that hold time counts here? Why not a finite floor for the core backstop (e.g. 60 s or 1 h) when the task has no timeout, so no IO can wait forever?
- Prelim: Q; risk M. Merges C5, B16.
- Status: RESOLVED (CHANGE)

Answer: KEEP 30 s, KEEP "0 = no timer", CHANGE nothing else (after D4). 30 s is the value Apple's AppleLSIFusionSAS publishes for the same family (design.md "Properties published") and macOS's convention; with D4 the 30 s counts from post like FreeBSD's 60 s does, and the measured worst latency at QD128 was 1.22 s (bench 2026-11-20), far below. "0 = infinite" is the family contract (`IOSCSIPrimaryCommandsDevice.cpp:304-306`): a FORMAT UNIT on an 18 TB disk legitimately runs for hours, so a finite floor would kill it. Comment to add at `mps_kext.cpp:478`: "timeout 0 = the family's 'no timeout' (long FORMAT/SANITIZE); FreeBSD's periphs always set one, the macOS clients don't".

Check: CHALLENGE on "0 = no timer" (30 s: accepted, see below). The macOS clients that send 0 are not only FORMAT/SANITIZE: SAM's own power management sends every START STOP UNIT with timeout 0 (`IOSCSIBlockCommandsDevicePM.cpp:288,424,439,489,516,550,676`). A lost reply to one of those (the case the backstop exists for, FINAL C1) hangs the PM thread for good: no sleep, no wake, a shutdown stall; FreeBSD never has an untimed ccb (da's spin-down SSU has a finite timeout). Proposal: keep the family contract (no family timer for 0) but give the core's FreeBSD callout a finite backstop for 0, e.g. 10 min, except for opcodes that legitimately run for hours (FORMAT UNIT 0x04, SANITIZE 0x48, WRITE SAME with long ranges if you want), which keep none. Host test: START STOP with timeout 0 and no reply -> recovery after the backstop. On 30 s: accept, but write the justification as "Linux sd default (SD_TIMEOUT 30 s) and the value macOS SAS HBAs publish; FreeBSD da uses 60 s"; the 1.22 s bench latency says nothing about a drive's error-recovery time, which is what the timeout must cover (enterprise SATA ERC is a few seconds, so 30 s is fine).
Answer (round 3): accept. Verified SAM sends START STOP UNIT with timeout 0 (`IOSCSIBlockCommandsDevicePM.cpp:288,424`, `SendCommand(request, 0)`); an untimed lost SSU reply would hang PM. Final fix: no family timer for 0 (family contract), but the core's FreeBSD callout gets a 10 min backstop for timeout 0 except FORMAT UNIT (0x04) and SANITIZE (0x48), which keep none. Host test: SSU with timeout 0 and no reply -> recovery after the backstop. 30 s comment: "Linux sd default (SD_TIMEOUT 30 s) and the value macOS SAS HBAs publish; FreeBSD da uses 60 s; it covers drive error recovery (enterprise SATA ERC is a few seconds)".

Check (round 4): Fix verified: family contract kept for 0, core backstop 10 min except FORMAT UNIT/SANITIZE, host test with an unanswered SSU; the 30 s comment text is accurate.

### D16 DELAY() of 1 ms or more drops mps_mtx (FreeBSD busy-waits with the lock held)
_(scan id B1)_
- Ours: `core/compat/mps_compat.c:531-555`, `core/compat/mps_compat.h:378-382`, `kext/mps_xnu_os.cpp:281-289`
- Reference: FreeBSD DELAY(9) spins and never releases locks; callers `mps/mps.c:953-964` (`mps_wait_db_int`: DELAY(2000) per handshake word, MPS_DB_MAX_WAIT 2500, `mps/mpsvar.h:41`), `mps.c` IOCFacts/IOCInit retry DELAY(1000), NO_SLEEP branches DELAY(50/100/256 ms)
- We do: DELAY(us >= 1000) looks up the first registered mtx owned by the caller, unlocks it, IOSleep(us/1000)+IODelay(rest), relocks. So `mps_wait_db_int` (`core/mps.c:1229-1241`) drops mps_mtx between handshake words, `mps_compat_callout_run` handlers and `cam_xnu_run_held` batches can lose atomicity.
- Reference does: atomic busy-wait; every FreeBSD caller that holds mps_mtx across DELAY assumes nothing else runs in between (that is why FreeBSD has the separate msleep/pause branches).
- Documented: design.md "Compat layer contract" DELAY row ("keeps the M4 rule no sleep with mps_mtx held"); FINAL L11 (downgraded, consequences M1/M2/H2).
- Question: xnu does not forbid blocking while holding a lck_mtx (IOLock is a sleeping mutex), so why not keep FreeBSD's semantics (sleep, but keep mps_mtx) for DELAY and rely on the explicit msleep/pause branches FreeBSD already has for the long waits? If dropping is kept, which call sites were audited for it (handshake in `mps_request_sync` vs `mps_xnu_check_fault`/`mps_periodic` doorbell reads, callout_drain == callout_stop)? Also: the 2500-iteration handshake budget becomes 2500 x (IOSleep(2) + scheduler/coalescing slack), not 5 s.
- Prelim: Q; risk M; deviation is a preference (M4 latency rule), not an xnu requirement; correctness depends on every DELAY caller tolerating a dropped lock.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (keep the lock across DELAY, as FreeBSD). The drop was the M4 "no sleep with mps_mtx held" preference (design.md:69), not an xnu rule: lck_mtx may be held across IOSleep. Dropping it silently breaks the atomicity every FreeBSD DELAY caller assumes (handshake in `mps_wait_db_int`, callout handlers, run_held batches).
Fix: DELAY = IODelay below 1 ms, IOSleep above, lock untouched; callers that FreeBSD already writes as msleep/pause keep dropping. Cost: the interrupt work loop waits on mps_mtx during a handshake (seconds at most, attach/reinit only), as FreeBSD spins. Host test: the virtual clock hook asserts the lock is still owned across DELAY; audit list in the commit: mps.c handshake, transition_ready, diag_reset NO_SLEEP branches.

Check: Accepted: DELAY keeps mps_mtx as FreeBSD; host hook asserts ownership across DELAY; worst hold is `mps_wait_db_int` 2500 x 2 ms per handshake word at attach/reinit, which is FreeBSD's spin budget.

### D17 CAM_DEV_QFRZN is released inside xpt_done; hence the per-TM wildcard-LUN freeze and lost retry order
_(scan id M5)_
- Ours: `core/compat/cam_compat.c:395-403` (QFRZN → release 1 on (tid,lun) at once, then hold/complete); core consequence `core/mps_sas.c:2509-2547` (`mpssas_prepare_for_tm`: path `tid, CAM_LUN_WILDCARD`, one freeze per TM, released in `mpssas_free_tm`), `:165-186`, `:1474-1478`; the error freeze of `mpssas_scsiio_complete` (`:1906-1907, 2096-2097`) is a no-op; `kext/mps_kext.cpp:140-162` (cbfcnp only queues)
- Reference: CAM `cam_xpt.c:5309-5320` (devq released only with CAM_DEV_QFRZDIS), `cam_periph.c:2111-2121` (periph re-issues the retry first, then `cam_release_devq`); `mps_sas.c:3251-3290` (freeze only (tid, lun), once, gated by INRESET; a new ccb per call)
- We do: the thin CAM plays the periph's release before anyone decides on a retry; the family retries much later behind younger IO; the TM freeze had to become wildcard and per-TM. INRESET is a per-target flag cleared by the first TM to free while another (OS TM, D10) may still hold a freeze.
- Reference does: the failed command is retried before IO queued behind it; one freeze per (tid,lun).
- Documented: design.md M5a-2 (wildcard freeze bullet); `cam_compat.h:14-18`; nothing in mps_kext.cpp.
- Question: IOKit has no periph to keep the freeze, so some release is needed. Could cam_compat instead map a (tid, lun) freeze made by a TM ccb onto the whole target and keep `mpssas_prepare_for_tm` verbatim? Is the ordering loss acceptable for SIMPLE-tagged disk IO (likely), and if so, say so in mps_kext.cpp? Is the devq freeze always enough to hold IO, i.e. does no path test INRESET without the freeze?
- Prelim: J (behaviour sound; deviation lives in core only because of a compat choice); risk M. Merges B2, C23, A25.
- Status: RESOLVED (KEEP (comment) + CHANGE (cosmetic))

Answer: KEEP (J): document. IOKit has no periph to hold a devq freeze until the retry is issued, so the thin CAM must release QFRZN at completion; the cost is that a failed SIMPLE-tagged IO is retried behind younger IO, which is harmless for disk READ/WRITE (no ordering contract between SIMPLE tasks). The wildcard per-TM freeze follows from that. No path tests INRESET without the devq freeze: checked `mpssas_action_scsiio` (`mps_sas.c:1474-1478`) is reached only through `xpt_action`, which holds when the devq is frozen. Comment for `mps_kext.cpp:140`: "QFRZN is released here (no periph); retries are re-ordered behind younger SIMPLE IO, which disks allow; TMs therefore freeze the whole target (mpssas_prepare_for_tm)". Mapping a (tid,lun) TM freeze onto the target in compat instead, to keep `mpssas_prepare_for_tm` verbatim: agree, CHANGE (cosmetic, low priority).

Check: Accepted: checked `cam_compat.c` xpt_action holds SCSI IO while the devq/simq is frozen and run_held re-checks per ccb, so INRESET is never the only guard; the proposed mps_kext.cpp:140 comment is accurate.

### D18 panic() returns; core continues past "impossible" SGL states and overruns the request frame
_(scan id B3)_
- Ours: `core/compat/mps_compat.c:266-277`, `kext/mps_xnu_os.cpp:543-548`; callers `core/mps.c:2760` (`mps_add_chain`), `core/mps.c:2850` (`mps_push_sge`)
- Reference: `mps/mps.c:2727, 2817` (panic, does not return)
- We do: log "panic: ..." + a fault line "KASSERT failed: ..." and return; `mps_add_chain`/`mps_push_sge` then write a 12/16-byte SGE with `cm_sglsize < MPS_SGC_SIZE`, i.e. past the request frame into the next SMID's frame.
- Reference does: stops the machine before the overrun.
- Documented: design.md "Rules for the port" (no panic) / `mps_os_assert_fail` comment; the two non-INVARIANTS panics are not discussed.
- Question: why not turn these two sites into an XNU error return (EFBIG -> requeue/fail the IO, or `mps_xnu_request_reinit`) or a real kernel panic, rather than silently corrupting the neighbouring frame? The `buffer_dmat` nsegments cap (`core/mps.c:1745`) makes it unreachable today; is there a test that asserts that?
- Prelim: Q; risk M; not reachable with the current nsegments cap, but the "never panic" rule converts a stop into memory corruption.
- Lead check: verified both sites (`core/mps.c:2759-2760`, `:2849-2850`); `mps_compat_panic` (`core/compat/mps_compat.c:266-277`) logs and returns.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Turning a stop into an overrun of the next request frame is the worst outcome. Fix: at the two sites (`mps.c:2760`, `:2850`) return EFBIG through the existing mapping-error path (the IO is failed/requeued, `mps_sas.c:1686-1706` already undoes) with a fault-level line; `mps_compat_panic` stays log+return for anything else. Host test: force a cm with nsegs > chain capacity -> EFBIG, no write past the frame (mock checks a guard pattern in the next frame). Also a unit test asserting the `buffer_dmat` nsegments cap (`mps.c:1745`) <= what `mps_add_chain` can hold.

Check: Accepted: EFBIG at both sites through the existing undo path (`mps_sas.c:1686-1706`), guard-pattern mock test and a static check that the nsegments cap fits the chain capacity.

### D19 Reinit retry: PCI state restored only after the first diag reset
_(scan id A10)_
- Ours: `core/mps.c:906-937`
- Reference: `mps.c:808-860` (one diag reset, `mps_pci_restore` at :844, panic at :856)
- We do: up to `MPS_XNU_REINIT_TRIES` (3) diag resets. `mps_pci_restore` and `mpssas_handle_reinit` run only when `attempt == 1`.
- Reference does: one attempt, so the case does not arise.
- Documented: design.md "Rules for the port" (≤ 3 attempts); the restore-once is not explained
- Question: if a diag reset can clear memory decode or bus mastering (design.md "Stubbed": "to be checked on hardware"), attempts 2 and 3 run without them. Their first doorbell read is then all-ones, which means GONE (D11), or IOCInit fails because the IOC cannot DMA. Why not call `mps_pci_restore` after every diag reset? It is idempotent.
- Prelim: U; risk L (only on nested faults); a cheap fix.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. No reason for `attempt == 1`; `mps_pci_restore` is idempotent. Fix: restore after every diag reset; keep `mpssas_handle_reinit` once. Host test: first diag reset fails, second succeeds -> restore hook called twice.

Check: Accepted: restore after every diag reset; host test counts the restore hook.

### D20 CAM status → family mapping: differences from cam_periph_error
_(scan id C13)_
- Ours: `kext/mps_kext.cpp:99-137`, `kext/SAS2308HBA.cpp:226-267`
- Reference: CAM `cam_periph.c:1923-2031`; Apple `IOBlockStorageServices.cpp:815-835` (retries any error except NotAttached/Offline, 4 times, at once).
- We do: (a) `CAM_AUTOSENSE_FAIL` → DeliveryFailure (retried). (b) `CAM_SCSI_BUS_RESET` on READ/WRITE → DeliveryFailure, which uses up one of SAM's 4 retries. (c) `CAM_DEV_NOT_THERE` → DeviceNotPresent only. (d) REQ_CMP_ERR, DATA_RUN_ERR, UNREC_HBA_ERROR, REQ_ABORTED → DeliveryFailure.
- Reference does: (a) EIO, not retried (`:1933-1935`). (b) retried without limit or count (`:1974-1995`). (c) ENXIO plus SSQ_LOST, i.e. `AC_LOST_DEVICE`, so the periph is invalidated at once (`:1960-1962`). (d) retried against the retry count (`:2008-2030`), equivalent to ours.
- Documented: design.md M5a-3 "Completion mapping". FINAL review called (d) correct.
- Question: (a) why retry an autosense failure? Map it to TASK_COMPLETE + CHECK_CONDITION without sense, or reject it. (b) why not complete reset-killed R/W with CAM_REQUEUE_REQ as M6 does for the rest (bounded by `xnu_resets`), so that no SAM retry is spent? (c) should a firmware DEVICE_NOT_THERE ask the core for a rescan of the tid, FreeBSD's AC_LOST_DEVICE, instead of waiting for a topology event?
- Prelim: (a) U, (b) U, (c) Q, (d) K; risk L.
- Lead check: (b) verified: `mpssas_xnu_reset_requeue` (`core/mps_sas.c:1410-1438`) returns 0 for READ/WRITE 6/10/12/16, so those complete `CAM_SCSI_BUS_RESET` (`:1882`) and spend a SAM retry.
- Status: RESOLVED (CHANGE (a,b,c) / KEEP (d))

Answer: (a) agree: CHANGE -> TASK_COMPLETE + CHECK_CONDITION with no sense (FreeBSD EIO, no retry). (b) agree: CHANGE -> complete reset-killed READ/WRITE with CAM_REQUEUE_REQ bounded by `xnu_resets` like the other commands (M6) so no SAM retry is spent; the READ/WRITE exclusion in `mpssas_xnu_reset_requeue` (`mps_sas.c:1410-1438`) was a precaution against replaying writes, but a requeue before the drive saw the command is exactly what CAM does. (c) agree: CHANGE -> DEVICE_NOT_THERE requests a rescan of that tid (FreeBSD's AC_LOST_DEVICE equivalent through AC_XNU_SCAN), still completing DeviceNotPresent. (d) KEEP, as reviewed. Host tests for (a)-(c) in mock_ioc_test.

Check: Accepted: matches cam_periph_error (autosense fail = EIO no retry; SCSI_BUS_RESET retried uncounted, `cam_periph.c` CAM_SCSI_BUS_RESET/BDR_SENT case; lost device -> rescan). Replaying a reset-killed WRITE is what CAM does too.

### D21 ReportMaximumTaskCount = normal frames − 8 vs FreeBSD SIM openings = normal frames − 1
_(scan id C12)_
- Ours: `kext/mps_kext.cpp:330-337`, `kext/SAS2308HBA.cpp:561-569`; core `mps_xnu.h:30` (`MPS_XNU_INTERNAL_CMDS 8`)
- Reference: FreeBSD `mps_sas.c:720-729` (`reqs = num_reqs - num_prireqs - 1`; `cam_simq_alloc(reqs)`, `cam_sim_alloc(..., reqs, reqs, ...)`). When it runs out, `mpssas_action_scsiio` requeues with the SIM frozen.
- We do: 8 frames are reserved for internal commands (events, acks, config pages, SATA IDENTIFY).
- Reference does: one reserve. Internal commands compete, and a starved IO is requeued by CAM, which the thin CAM also implements.
- Documented: design.md "Queue sizes as FreeBSD" ("8 internal, as before"), which gives no reason.
- Question: why not FreeBSD's −1, since the requeue path exists?
- Prelim: U; risk L.
- Also (scan id A21 = D58): core side `core/mps.c:396-400`, `core/mps_xnu.h:30` (`MPS_XNU_INTERNAL_CMDS 8`); the SIM queue depth in `core/mps_sas.c:560` is still FreeBSD's `num_reqs - num_prireqs - 1`, so the thin CAM's openings and the family pool differ by 7.
- Status: RESOLVED (CHANGE)

Answer: KEEP the 8-frame reserve (J: justify), CHANGE the inconsistency. Reason: internal commands (event ACK, config pages, SATA IDENTIFY during a hot-add) draw from the same normal pool; with FreeBSD's -1, a full IO load starves them and a hot-add's page-0 read fails (FreeBSD's `mps_config_*` returns EBUSY with no cm and the add is dropped), while an IO that misses a frame is merely requeued. 8 of 2048 frames costs nothing (2040 tasks measured, no throughput change). CHANGE: make the SIM queue depth (`mps_sas.c:560`) equal to `xnu.io_cmds` so the thin CAM and the family agree, with a `/* XNU: */` comment saying why.

Check: CHALLENGE (where, not whether). The reserve is defensible (FreeBSD's own `mps_alloc_command` can starve config/IDENTIFY under full load; FreeBSD gives CAM every normal frame, `mps_sas.c:720`). But the proposed CHANGE moves the core further from FreeBSD: `mps_sas.c:560` is FreeBSD's line and the thin CAM does not enforce openings anyway (D65, `cam_compat.c` stores them only). Counter-proposal: leave `mps_sas.c:560` verbatim, delete `xnu.io_cmds` and its lines from `mps.c:396-400`, and compute the family's task count in the kext (`mps_kext_max_tasks` = normal frames - MPS_XNU_INTERNAL_CMDS) with the `/* XNU: */` reason there: one XNU line in the glue instead of two in the core. Also state in the comment what the 8 cover (config/IDENTIFY: 1 at a time on the event queue; port enable 1; REMOVE_DEVICE 1 per pending removal; event ACKs pending D29).
Answer (round 3): accept. Final fix: `mps_sas.c:560` and `mps.c:396-400` back to FreeBSD (delete `xnu.io_cmds`); `mps_kext_max_tasks` = normal frames - MPS_XNU_INTERNAL_CMDS with the `/* XNU: */` reason in the glue: the 8 cover config/IDENTIFY (1 at a time on the event queue), port enable (1), REMOVE_DEVICE (1 per pending removal) and pending event ACKs (D29).

Check (round 4): Fix verified: core lines back to FreeBSD, the reserve and its reason live in the glue only.

### D22 Default log mask much wider than FreeBSD's (info,fault)
_(scan id M3)_
- Ours: `core/mps.c:1838` (`mps_xnu_level_mask(MPS_LEVEL_INFO)`), `core/mps_xnu.c:33-47` (level 2 = fault,error,recovery,info,init,event,mapping), `kext/Info.plist:47-48` (`SAS2308LogLevel` 2), `kext/mps_xnu_os.cpp:97-122` (event/mapping/init/recovery → os_log DEFAULT, persisted), `core/mpsvar.h:691-693` (`mps_print_field` classed `MPS_EVENT|MPS_XINFO`)
- Reference: `mps.c:1575` (`sc->mps_debug = MPS_INFO|MPS_FAULT`)
- We do: by default every firmware event is dumped field by field (`MPS_DPRINT_EVENT` gates on MPS_EVENT: 5-10 lines per topology list plus a line per PHY), `MPS_INIT` prints "Doorbell= 0x%x" on every 50 ms poll of `mps_transition_ready` (attach, reinit, suspend), every TM's "Freezing/Unfreezing devq" (tagged XINFO|RECOVERY, counted as RECOVERY), and every per-IO `MPS_ERROR` "NULL handle for target %u in diag reset freezing queue" (`mps_sas.c:1481`, FreeBSD `:1637`) during a reinit.
- Reference does: none of these at the default level; CAM periphs print IO errors.
- Documented: design.md "Logging (M4)" levels table (it says field dumps are xinfo, which is not what `MPS_DPRINT_EVENT` does); no comparison with FreeBSD's default.
- Question: why is the default not FreeBSD's `info,fault` (plus `error` for IO failures, which no CAM periph prints on macOS)? Why do EVENT/MAPPING/INIT/RECOVERY persist by default, and why does a line tagged XINFO|RECOVERY count as DEFAULT rather than DEBUG? Separate from the check-condition level on branch log-noise.
- Prelim: U (keep levels as opt-in, change the default); risk L (log volume; buries real errors; msgbuf churn). Merges A11, B17, C8.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Default = FreeBSD's info|fault plus error (macOS has no CAM periph printing IO errors, so error stays on); event/mapping/init/recovery become opt-in levels. Also fix design.md's table (field dumps are MPS_EVENT-gated, not xinfo) and make a line tagged XINFO|anything count as DEBUG (most-verbose class wins), so "Freezing devq" stops persisting. Check against log-noise's change so the two don't conflict.

Check: Accepted: default info|fault|error; design.md table fixed; XINFO-tagged lines go to DEBUG (checked: no line combines XINFO with FAULT or ERROR, so nothing serious is demoted). Coordinate with log-noise.

### D23 Log lines the kext adds on top of the core's
_(scan id C9)_
- Ours: `kext/SAS2308Glue.cpp:278-281` (every synchronously refused IO, ERROR class, after the core already logged `NULL handle`/gone); `:305-306` ("timeout for a task that is not outstanding", RECOVERY|ERROR: the normal race where the completion is queued but not yet delivered); `:428-436` (AC_BUS_RESET / AC_SENT_BDR lines that repeat the core's reinit and TM lines); `kext/SAS2308HBA.cpp:745,777` (two RECOVERY lines per OS TM); `kext/SAS2308HBA_Props.cpp:146` (a mask change logged at ERROR); `kext/SAS2308Glue.cpp:218,249`
- Reference: FreeBSD `mps_sas.c:1553-1554` ("almost timed out" at MPS_XINFO), `:1644` (one MPS_ERROR line per refused IO), `:999` (XPT_RESET_DEV at MPS_XINFO)
- We do: two lines per event, at a higher class than FreeBSD.
- Reference does: one line, often XINFO.
- Documented: no.
- Question: why not drop the kext duplicates, or lower them to XINFO, so that the core's FreeBSD lines are the only record (the refused-IO line also floods at ERROR during a removal or after shutdown)?
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Drop the kext's refused-IO line (`SAS2308Glue.cpp:278-281`) when the core already logged it, lower "timeout for a task that is not outstanding" to XINFO (normal completion race), drop the AC_BUS_RESET/AC_SENT_BDR lines (core logs both), one RECOVERY line per OS TM, mask-change line to INFO (it was ERROR only so it would persist; INFO persists too, measured today).

Check: Accepted as listed.

### D24 os_log type derived from the union of classes; MPS_FAULT is ERROR, not FAULT
_(scan id B18)_
- Ours: `kext/mps_xnu_os.cpp:97-122, 138`; category `core/mps_xnu.c:76-92`
- Reference: none (FreeBSD has no levels)
- We do: ERROR if any of FAULT|ERROR, DEFAULT if any info-ish class, else DEBUG; category = "most specific" class with a different precedence than the type.
- Reference does: n/a.
- Documented: design.md "Logging (M4)".
- Question: why not OS_LOG_TYPE_FAULT for MPS_FAULT (IOC fault, controller disabled), which is what os_log fault exists for? Should type follow the same precedence as the category so one line is consistent?
- Prelim: Q; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. OS_LOG_TYPE_FAULT for MPS_FAULT (IOC fault, controller disabled, surprise removal) is what fault is for; type and category from the same precedence (most specific class wins for both).

Check: Accepted, with one exception: the surprise-removal line (`mps_xnu_gone`) is an expected user action (dock unplug) and should stay ERROR; FAULT for IOC FAULT and controller disabled.

### D25 Lines without a device bypass IOLog mode and lose the unit
_(scan id B19)_
- Ours: `kext/mps_xnu_os.cpp:124-139, 141-144, 543-548`; `core/compat/mps_compat.c:149-175, 247-252`
- Reference: FreeBSD printf/KASSERT go to the console/msgbuf like device_printf
- We do: `printf`, `hexdump`, KASSERT/panic and thin-CAM warnings ("SIM queue released too often") use dev NULL -> host NULL -> always os_log, unit 0, even with `sas2308_iolog=1`.
- Reference does: same sink as everything else.
- Documented: no.
- Question: with iolog on (the mode meant to survive into panic logs), KASSERT lines are exactly the ones you want there; why not fall back to the last/only host, or a global iolog flag?
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Fall back to the only/last attached host's iolog flag (a global set by the first attach) so KASSERT/printf lines follow `sas2308_iolog=1` and carry a unit.

Check: Accepted.

### D26 Failed-IO line and rate limiter (new): per-slot hash collisions
_(scan id A30)_
- Ours: `core/mps_xnu.c:728-875`, `:398-432`, `core/mps_xnu.h:85-100`
- Reference: `mps_sas.c:1904` (`mps_sc_failed_io_info`, MPS_XINFO lines, no rate limit)
- We do: an MPS_ERROR line per failed IO, 5 per 10 s per slot, slot = `handle % 8`. Different targets share a window, and the "N suppressed" summary names the last target that used the slot.
- Reference does: silent at the default level.
- Documented: design.md "Logging (M4) / Rate limiting"
- Question: (the check-condition level is skipped, it is on branch log-noise.) Why hash on handle, rather than keep the limiter in `struct mpssas_target`, so it is per target and exact?
- Prelim: Q; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Move the limiter into `struct mpssas_target` (exact per target); keep the "N suppressed" summary per target. Coordinate with log-noise, which already moved the failed-IO line for SCSI-status completions to xinfo (the limiter then matters only for real failures).

Check: Accepted: limiter in `struct mpssas_target`.

### D27 Logging plumbing: mps_dprint keeps its classes; printf/device_printf at class 0; %b replaced
_(scan id A33)_
- Ours: `core/mpsvar.h:691-712`, `core/mps_xnu.c:76-93` (category = most specific class), `core/mps_xnu.c:117-150`, `core/mps_xnu.h:67` (`MPS_XNU_BITS`, compound-literal buffer per call)
- Reference: `mpsvar.h:680` (`mps_dprint` -> `device_printf`), kernel `%b`
- We do: os_log categories per class; `%b` emulated.
- Reference does: plain `device_printf`.
- Documented: design.md "Logging (M4)", M5a-3 "Platform"
- Question: `%b` could be handled in compat's vsnprintf wrapper (`mps_compat_vlog`) so every `%b` call site stays verbatim. Why patch 8 sites instead? `mps_table.c` is then unchanged except the event names.
- Prelim: U (move to compat); risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Handle `%b` in compat's `mps_compat_vlog` (FreeBSD kernel printf semantics) and revert the 8 call sites to verbatim.

Check: Accepted: `%b` in `mps_compat_vlog`, 8 sites back to verbatim.

### D28 EEDP (T10 PI) removed; the code comment claims eedp_enabled is 0, which is false
_(scan id A12)_
- Ours: `core/mps_sas.c:1651` ("sc->eedp_enabled is 0"), `:616,650` (no `mpssas_async`); `core/mps.c:630-631` sets `eedp_enabled = TRUE` when IOCCapabilities has EEDP (the SAS2308 does)
- Reference: `mps_sas.c:1802-1850` (EEDP flags, ref-tag insert for READ/WRITE on PI-formatted LUNs), `:3138-3250` (`mpssas_async` -> `mpssas_check_eedp` from READ CAPACITY(16) through `AC_ADVINFO_CHANGED`)
- We do: no protection-information offload. A PI-formatted SAS disk gets plain READ/WRITE (RDPROTECT/WRPROTECT 0): it works, without end-to-end checking.
- Reference does: HBA-generated and HBA-checked PI on PI-formatted LUNs.
- Documented: design.md M5a-3 "Not ported: … EEDP"; the code comment is wrong
- Question: fix the comment. Is EEDP wanted at all? It could be ported by snooping READ CAPACITY(16) completions in `mpssas_scsiio_complete` instead of using CAM advinfo.
- Prelim: J (omission fine; comment wrong); risk L.
- Status: RESOLVED (KEEP (comment fix))

Answer: KEEP the omission, CHANGE the comment. EEDP is not wanted now: the drives are SATA (no PI), and porting needs the READ CAPACITY(16) snoop. Comment at `mps_sas.c:1651`: "XNU: EEDP (T10 PI offload) not ported: no CAM advinfo to learn a LUN's PI format; eedp_enabled is set from IOCCapabilities but unused". Revisit when a PI-formatted SAS disk is attached.

Check: Accepted: the proposed comment is accurate (`eedp_enabled` set at `mps.c:630-631`, unused).

### D29 EVENT_ACK sent for AckRequired events (FreeBSD never acks)
_(scan id A14)_
- Ours: `core/mps.c:2574-2579`, `core/mps_xnu.c:563-610`
- Reference: none (`grep EVENT_ACK` finds nothing; only `mps_table.c:306` prints AckRequired)
- We do: an EVENT_ACK on a normal command per AckRequired event. If no command is free the ack is dropped and counted.
- Reference does: never acks.
- Documented: code comment (M1-M4, Linux mpt2sas does it); design.md "Stubbed" (dropped acks)
- Question: has the 9207 IT firmware ever set AckRequired (check the stat on hardware)? If it never does, this is dead code. If it does, is FreeBSD wrong, or does the firmware not care?
- Prelim: Q; risk L.
- Status: RESOLVED (HW-TEST)

Answer: OPEN-NEEDS-HW-TEST, KEEP meanwhile. The MPI2 spec asks the host to ACK events with AckRequired; Linux mpt2sas does; FreeBSD doesn't, and the firmware evidently tolerates that. Test: read the event-ACK counter in SAS2308Statistics after boot + the pending hot-plug tests (drive pull/insert). If it stays 0 on the 9207 IT firmware -> remove (back to FreeBSD); if not -> keep with a comment citing the observed events.

Check: Accepted, with a precondition: there is no "acks sent" counter today (only `event_ack_dropped`, `core/mps_xnu_stats.h:49`, published as "Event acks dropped"); add "Event acks sent" (and log the event code once) before the test, else a 0 proves nothing.

### D30 Doorbell FAULT check on every interrupt that finds an empty queue
_(scan id A15)_
- Ours: `core/mps.c:2542-2544`, `core/mps_xnu.c:650-665`
- Reference: `mps.c:2103-2120` (`mps_periodic` only, 1 s)
- We do: one extra uncached MMIO read over Thunderbolt per empty interrupt (MSI races, polled callers `mps_xnu_ccb_timeout`, the suspend drain, `mpssas_poll`).
- Reference does: a fault is found within 1 s by the periodic check.
- Documented: design.md M5a-2 ("M2-M4")
- Question: what does it buy over `mps_periodic` (at most 1 s sooner)? Is the cost measured at high IOPS with MSI?
- Prelim: Q; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Remove the per-empty-interrupt doorbell read; `mps_periodic` (1 s, FreeBSD) finds a fault. The only gain is <= 1 s earlier detection; the cost is an uncached read over Thunderbolt per spurious interrupt.

Check: Accepted: remove the per-empty-interrupt doorbell read; `mps_periodic` (1 s) covers faults as on FreeBSD.

### D31 IOC temperature poll (IO Unit Page 7 every 30 s): new code with a 60 s blocking failure mode
_(scan id A16)_
- Ours: `core/mps_config.c:38-155`, `core/mps_xnu.c:1131-1224`, `core/mps_sas_lsi.c:324-326`
- Reference: none in FreeBSD mps
- We do: every 30 s a config read on the event taskqueue, through `mps_wait_command(…, 60, …)`. If the IOC does not answer, the event queue is blocked for 60 s, then `mps_reinit` runs inline: a full diag reset caused by a monitoring read. It also uses the single page buffer (D6), so D2 kills it.
- Reference does: N/A.
- Documented: design.md "M5c / IOC temperature"
- Question: is a periodic firmware request worth a possible controller reset? Why not a shorter timeout with no reinit for this read (`cm_timeout_handler`, as the ATA ID read does), or read only on demand (`sas2308ctl stats`)?
- Prelim: Q; risk L.
- Also (scan id C27 = D81): should the poll be off by default (FreeBSD sends no periodic config requests), read only on `sas2308ctl stats` / property read?
- Status: RESOLVED (CHANGE)

Answer: KEEP the poll (the temperature readout is a requested feature, for trends), CHANGE its failure mode. Fix: this read uses a short timeout (5 s) with its own timeout handler that marks the read failed and counts it (like the ATA ID path), never `mps_reinit`; skip polls while a reinit/discovery is pending; with D6 it no longer competes for one buffer. Keep it periodic at 30 s (the stats sampler and the temperature property depend on it).

Check: CHALLENGE (failure mode). "Short timeout, never `mps_reinit`" leaks a normal command each time: a config request that times out stays owned by the IOC (INQUEUE) and nothing reclaims it, unlike the IDENTIFY case where FreeBSD kicks it with a target reset (D2); there is no target to reset for an IOC page. At one poll per 30 s a wedged-but-not-faulted IOC eats the 8-command reserve (D21) in 4 minutes and then starves config/IDENTIFY/REMOVE. A config timeout means a wedged IOC; FreeBSD's answer for every config read is `mps_wait_command` -> reinit. Proposal: after the first poll timeout, stop polling (one leaked cm at most, reclaimed by the next reinit), count it, log once; keep the 5 s timeout so the event queue is not blocked for 60 s. Was the readout requested as a feature? If not, off by default.
Answer (round 3): accept. Final fix: 5 s timeout, no reinit; after the first poll timeout stop polling (at most one leaked cm, reclaimed by the next reinit), count it, log once at error; polling resumes after a reinit. Confirmed: the readout was requested (does the card throttle?), implemented in c5012bd ("IOC Temperature (C)" + SAS2308Statistics), so on by default.

Check (round 4): Fix verified: short timeout, no reinit, polling stops after the first timeout (one cm at most until the next reinit), resumes after reinit; feature request confirmed (c5012bd).

### D32 maxio: MPS_XNU_MAXIO_PAGES 256 in the core, and a hard-coded 1 MiB in the kext
_(scan id M8)_
- Ours: `core/mps.c:1843`, `core/mps_xnu.h:31` (`MPS_XNU_MAXIO_PAGES 256`), compat `core/compat/mps_compat.h:103-104` (`PAGE_SIZE 4096`, `maxphys 1 MiB`); kext `kext/SAS2308HBA.cpp:360-371` (`1024*1024` literal, capped by `(segs-1)*PAGE_SIZE` with xnu's 16 KiB PAGE_SIZE)
- Reference: `mpsvar.h:48` (`MPS_MAXIO_PAGES (-1)`), `mps.c:405-431` (maxio = HW limit from frame size × MaxChainDepth; chains sized for min(HW, maxphys)); `mps_sas.c:944` (`cpi->maxio = sc->maxio`, the periph caps at maxphys)
- We do: two independent 1 MiB constants; the tunable is not the single knob.
- Reference does: -1 → chains for maxphys (compat already makes that 1 MiB); `sc->maxio` = HW max (about 7.5 MiB) reported, periph clamps.
- Documented: design.md "Rules for the port" (tunable defaults), M5a-3 ("≤ buffer_dmat nsegments").
- Question: since compat's `maxphys` already gives the same sizing, why not keep FreeBSD's -1, export `sc->maxio` through `mps_kext_limits`, and let the kext clamp it to its own maxphys-equivalent? (Only difference: `buffer_dmat` nsegs becomes about 1922, which sizes the per-task segment array: check the task-data cost before changing.)
- Prelim: U (with the task-data size check); risk L (same value today). Merges A22, C19.
- Status: RESOLVED (CHANGE)

Answer: KEEP 1 MiB, CHANGE to one knob. A FreeBSD -1 would size `buffer_dmat` for ~1922 segments and the family's per-task segment list with it: today's 2040 tasks x 4648 B = 9 MiB already (bench 2026-11-20); x7 would be ~60 MiB wired, for IOs macOS never sends (IOBlockStorageDriver caps at 1 MiB by default). Fix: keep FreeBSD's `MPS_MAXIO_PAGES (-1)` semantics with compat `maxphys` = 1 MiB as the single knob, export `sc->maxio` via `mps_kext_limits`, and replace the kext's `1024*1024` literal with it.

Check: CHALLENGE (numbers and fix). With 128-byte frames: sges_per_frame = 128/12 - 1 = 9, FreeBSD maxio = (9 x 128 + 1) x 4 KiB = 4.5 MiB (`mps.c:412-413`), buffer_dmat nsegs = 1154 (`mps.c:1494`), not ~1922; the task data would be about 2040 x (1154 x 16 + ~500) = ~38 MiB, not ~60 (my A22 figure was wrong too). More important: the proposed fix ("keep FreeBSD's -1, export sc->maxio") produces exactly that 4.5 MiB / 1154 segments, the cost the answer rejects, unless the kext clamps its per-task segment array separately. Simplest FreeBSD-faithful version: keep `max_io_pages` = 256 as a tunable *value* (FreeBSD supports `hw.mps.max_io_pages`; with D33 it becomes `mps.max_io_pages=256` in the personality, the core line back to FreeBSD's `MPS_MAXIO_PAGES`), export `sc->maxio` via `mps_kext_limits`, replace the kext's `1024*1024` literal. One knob, no core XNU line, same memory as today.
Answer (round 3): accept, numbers included (4.5 MiB / 1154 segs / ~38 MiB; my ~60 was wrong). Final fix: core line back to FreeBSD's `MPS_MAXIO_PAGES`; `max_io_pages` = 256 as a tunable value (`mps.max_io_pages` via D33's TUNABLE_INT_FETCH, default in the personality); export `sc->maxio` via `mps_kext_limits`; replace the kext's `1024*1024` literal with it. One knob, same memory as today.

Check (round 4): Fix verified: no core XNU line, 256 as a tunable value through D33, kext publishes `sc->maxio`; memory unchanged.

### D33 mps_get_tunables: no hw.mps.* tunables; only the debug mask can be overridden
_(scan id A26)_
- Ours: `core/mps.c:1831-1852`; kext `kext/mps_kext.cpp:249`
- Reference: `mps.c:1570-1660` (`TUNABLE_INT_FETCH` for disable_msi(x), max_chains, max_io_pages, enable_ssu, spinup_wait_time, use_phy_num, max_*frames, exclude_ids)
- We do: hard defaults. `exclude_ids`, `enable_ssu` and `use_phynum` are kept in the softc but cannot be set or are unused.
- Reference does: loader tunables per driver and per unit.
- Documented: code comment ("the kext applies its Info.plist properties and boot-args after this")
- Question: why not keep `mps_get_tunables` verbatim? A compat `TUNABLE_INT_FETCH` / `TUNABLE_STR_FETCH` could read `sas2308.<name>` boot-args or personality properties. That restores field tuning (spinup_wait_time, max_reqframes) without code changes.
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Keep `mps_get_tunables` verbatim; compat `TUNABLE_INT_FETCH`/`TUNABLE_STR_FETCH` read `mps.<name>` / `mps.<unit>.<name>` from boot-args (PE_parse_boot_argn) then the personality, so spinup_wait_time/max_reqframes/exclude_ids work without code changes.

Check: Accepted: compat TUNABLE_*_FETCH from boot-args then personality; `mps_get_tunables` verbatim.

### D34 Suspend / shutdown core machinery and its constants
_(scan id A28)_
- Ours: `core/mps.c:1028-1132`, `:218-223` (MUR ack 1 s at shutdown), `:254-316` (`stop_fast`, `stop_deadline_us`), `core/mps_xnu.h:40-44` (drain 1 s, stop 2 s / 10 s, reinit stop 2 s, resume 20 × 100 ms)
- Reference: none in FreeBSD (suspend and resume return EINVAL; shutdown only through `mpssas_ir_shutdown`)
- We do: hold new IO (sleep) or fail it (shutdown), drain, MUR, complete what is left as after a reset (sleep) or fail it (shutdown), refuse register access while suspended.
- Reference does: N/A.
- Documented: design.md "M5b Power management", "Review fixes" H5/M1/M4/M8
- Question: all constants are untested guesses (design.md: "untested on hardware"). Is the 1 s shutdown drain enough for SYNCHRONIZE CACHE on 7 or more spinning disks with a write cache? macOS issues it before halt. See also D9.
- Prelim: Q; risk L (needs the hardware sleep and shutdown plan).
- Status: RESOLVED (HW-TEST)

Answer: OPEN-NEEDS-HW-TEST (partly done). Done: `shutdown -r` with mounted JHFS+ volumes and unsynced writes, fsck clean, no hang (measured on the test setup); sleep/wake under IO (498 ms). Still needed: shutdown while all 7 drives write at full rate with write cache on, then verify-pass the written ranges after boot (0 mismatches) and check whether macOS's SYNCHRONIZE CACHE reached each drive before the MUR (debug trace). Constants stay until that run.

Check: Accepted: the full-rate shutdown + verify run and the SYNCHRONIZE CACHE trace are the right test; constants stay until then.

### D35 xpt_done() runs the held queue synchronously inside the SIM's completion path; O(N_held) per release
_(scan id B8)_
- Ours: `core/compat/cam_compat.c:404-416, 239-268`
- Reference: `cam/cam_xpt.c:5309-5312` (xpt_release_simq(run_queue=FALSE) in the done thread), then `xpt_run_devq` which stops at the first frozen queue
- We do: CAM_RELEASE_SIMQ/QFRZN on a completed ccb immediately calls `cam_xnu_run_held`, i.e. `mpssas_action_scsiio` re-entered from `mpssas_scsiio_complete`/`mpssas_complete_all_commands`/`mps_intr_locked`. The batch walk re-checks and re-holds every held ccb even after the first one re-froze the SIM.
- Reference does: completions never re-enter the SIM's action; xpt_run_devq does O(1) work while the SIM queue is frozen.
- Documented: no (contract row says "re-sent on release").
- Question: during `mpssas_complete_all_commands` with ~2000 posted and many held, each completion carrying CAM_RELEASE_SIMQ re-runs the whole held list (first ccb re-freezes via DIAGRESET, the rest are re-held one by one): O(N_posted x N_held) under mps_mtx. Why not stop the batch (splice the rest back) as soon as `sim->freeze > 0`, and/or defer the run to the end of the completion pass?
- Prelim: Q; risk L; perf/latency under mps_mtx during reinit, plus re-entrancy FreeBSD does not have.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Stop the batch as soon as `sim->freeze > 0` (splice the rest back in order) and defer the held-queue run to the end of the completion pass (set a flag in xpt_done, run once after the pass), which is CAM's done-thread behaviour. Host test: reinit with 2000 posted + 500 held -> run_held work bounded (count calls).

Check: Accepted: stop the batch at `sim->freeze > 0`, run once per completion pass; bounded-work host test.

### D36 AC_LOST_DEVICE registered by the kext but never posted
_(scan id B13)_
- Ours: `kext/mps_kext.cpp:195-198, 264`; `kext/mps_xnu_os.h:71`; no `xpt_async(AC_LOST_DEVICE` in core
- Reference: AC_LOST_DEVICE comes from CAM's probe/invalidations, never from mps
- We do: dead branch and enum value.
- Reference does: n/a.
- Documented: no.
- Question: remove it (or document that removals arrive only as AC_XNU_SCAN)?
- Prelim: U; risk L; dead code that suggests a path that does not exist.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Remove the dead AC_LOST_DEVICE branch and enum; a comment in `mps_kext.cpp` says removals arrive only as AC_XNU_SCAN (and, after D20(c), DEVICE_NOT_THERE triggers that scan).

Check: Accepted.

### D37 Several "FreeBSD values" in cam_compat.h are not FreeBSD's
_(scan id B14)_
- Ours: `core/compat/cam_compat.h:110-123` (XPT_*), `:130` AC_ADVINFO_CHANGED 0x8000, `:134` CAM_PRIORITY_NORMAL 5
- Reference: `cam/cam_ccb.h:139-221` (XPT_SCAN_BUS 0x08|..., XPT_SCAN_LUN 0x14|..., XPT_SCSI_IO 0x01|XPT_FC_DEV_QUEUED), AC_ADVINFO_CHANGED 0x2000, CAM_PRIORITY_NORMAL ((CAM_RL_NORMAL<<8)+0x80)
- We do: different numbers (our XPT_SCAN_BUS 0x14 = FreeBSD's XPT_SCAN_LUN low byte); all status/flag values do match (checked all 46).
- Reference does: as listed.
- Documented: header claims "Values and field names follow FreeBSD" (`cam_compat.h:5-7`).
- Question: copy the real values (or drop the claim); nothing depends on them today.
- Prelim: U; risk L; harmless, but misleading for anyone diffing against CAM.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Copy CAM's real values for XPT_* codes, AC_ADVINFO_CHANGED and CAM_PRIORITY_NORMAL (nothing depends on them), so the header's "values follow FreeBSD" claim is true.

Check: Accepted.

### D38 malloc(9): canwait ignored, M_WAITOK can return NULL, header in front of every block
_(scan id B21)_
- Ours: `core/compat/mps_compat.c:338-368`, `kext/mps_xnu_os.cpp:202-211`
- Reference: malloc(9): M_WAITOK never fails, M_NOWAIT never sleeps
- We do: IOMalloc for both (may block under mps_mtx; may return NULL for WAITOK); 16-byte size/magic header.
- Reference does: as above.
- Documented: design.md contract row; FINAL L13 (partly fixed: unchecked WAITOK results).
- Question: blocking under a lck_mtx is legal; is there any M_NOWAIT caller that runs in a context where blocking is not (interrupt filter, preemption disabled)? If none, write that in the hook comment.
- Prelim: J; risk L.
- Status: RESOLVED (KEEP (comment))

Answer: KEEP (J), CHANGE the comment. No M_NOWAIT caller runs where blocking is illegal: the core runs in thread context only (work-loop interrupt handler thread, thread_calls, taskqueue, client threads); the family's primary-interrupt filter doesn't enter the core. Comment for the malloc hook: "IOMalloc may block; legal: no core path runs at primary interrupt level or with preemption disabled; M_WAITOK NULL is checked by every caller (FINAL L13)".

Check: Accepted: checked `FilterInterruptRequest` returns true without entering the core (`kext/SAS2308HBA.cpp:667-670`); all core entry is thread context. The proposed comment is accurate.

### D39 Taskqueue on a thread_call; drain polls; second invocation returns
_(scan id B24)_
- Ours: `core/compat/mps_compat.c:688-793`, `kext/mps_xnu_os.cpp:491-535`
- Reference: taskqueue(9) with one kthread (`taskqueue_start_threads(..., 1, PRIBIO, ...)`), drain sleeps until the task is idle
- We do: thread_call (HIGH group, FINAL L10); `tq_active` serializes; drain = kick + IOSleep(1) loop (never returns on a stopped queue, FINAL L9); free runs leftovers inline in the caller.
- Reference does: dedicated thread at PRIBIO; drain wakes on completion.
- Documented: design.md contract row; code comment `mps_compat.c:745-752`.
- Question: the reinit task can run for minutes (diag reset up to 300 s) on a HIGH-priority thread_call worker; why not a dedicated kernel thread (kernel_thread_start is public) which is exactly FreeBSD's model and would give a real drain (sleep/wakeup)?
- Prelim: Q; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (medium priority). A dedicated kernel thread (`kernel_thread_start`, KPI) per taskqueue is FreeBSD's model, gives a real sleep/wakeup drain (fixes FINAL L9) and stops a minutes-long reinit from occupying a HIGH thread_call worker (FINAL L10).

Check: Accepted: a dedicated kernel thread per taskqueue (FreeBSD's model; real drain, FINAL L9/L10).

### D40 config_intrhook: global list run from StartController
_(scan id B25)_
- Ours: `core/compat/mps_compat.c:797-848`; caller `kext/SAS2308HBA.cpp:398`
- Reference: config_intrhook_establish(9): after boot the hook runs from a taskqueue once interrupts work
- We do: list run by `mps_compat_intrhook_run()` (runs every HBA's hooks, FINAL L16).
- Reference does: per-hook, asynchronous.
- Documented: design.md contract row.
- Question: run only the caller's hook (pass the device) so a second HBA cannot run the first one's `mps_startup` before its EnableInterrupt?
- Prelim: J; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Run only the caller's hooks (pass the device), so a second HBA can't run the first one's `mps_startup` before its EnableInterrupt.

Check: Accepted.

### D41 bus_dma tags: boundary neither inherited nor enforced; lowaddr/alignment only at alloc
_(scan id B26)_
- Ours: `core/compat/mps_compat.c:861-960`, `kext/mps_xnu_os.cpp:349-404`, `core/compat/cam_compat.c:573-604`
- Reference: bus_dma_tag_create (boundary = min of non-zero parent/child, lowaddr min, alignment max); `_bus_dmamap_load_*` split segments at boundary/maxsegsz and bounce above lowaddr
- We do: `dt_boundary` stored but never checked (chain_dmat/buffer_dmat ask for 4 GiB boundary, `core/mps.c:1700, 1752`); dmamem alloc checks alignment/lowaddr/maxsegsz per segment; `bus_dmamap_load_ccb` checks nsegments/maxsegsz/total only.
- Reference does: enforces all tag constraints at load.
- Documented: FINAL M5 (open) for the ccb side; nothing for dmamem.
- Question: dmamem tags are all 32-bit lowaddr so the boundary cannot be crossed there; why not enforce `dt_boundary` generically in `bus_dmamap_load_ccb` (refuse or split) so M5 is fixed in compat rather than in the kext?
- Prelim: Q; risk L (ccb side tracked as FINAL M5).
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Enforce `dt_boundary` in compat `bus_dmamap_load_ccb` (split a segment at the boundary when nsegments allows, else EFBIG -> requeue), which fixes FINAL M5 (4 GiB crossing) in compat instead of the kext; inherit boundary/lowaddr/alignment from the parent tag as FreeBSD.

Check: Accepted: boundary enforced in compat load_ccb (fixes FINAL M5 there), parent-tag inheritance.

### D42 bus_dmamap_load error contract differs from FreeBSD
_(scan id B27)_
- Ours: `core/compat/mps_compat.c:1029-1079`
- Reference: busdma `bus_dmamap_load`: on error calls the callback with (segs, 0, error) and returns 0, except ENOMEM (returned); deferred EINPROGRESS
- We do: callback with error and return the error (EINVAL/EFBIG); ENOMEM from the seg-list allocation returns without callback; only dmamem buffers (or a lent map) can be loaded.
- Reference does: as above; any kernel buffer.
- Documented: header comment `mps_compat.h:539-549`; FINAL L14 (fixed part).
- Question: core callers ignore the return for queue/reply/request loads; return 0 like FreeBSD for non-ENOMEM errors to keep caller logic verbatim?
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Follow busdma: callback with the error and return 0, except ENOMEM returned; keeps the core's callers verbatim.

Check: Accepted.

### D43 Reply-queue pumping and delivery from extra threads (TM waiter, target drain, statistics timer)
_(scan id C7)_
- Ours: `kext/mps_kext.cpp:605-609` (`mps_intr_locked` every 10 ms in the TM waiter), `kext/mps_kext.cpp:723-727` (the same in `mps_kext_target_drain`), `kext/SAS2308HBA_Props.cpp:482` (`DeliverCompletions` every second, "belt and braces")
- Reference: FreeBSD drains the reply queue only from the interrupt and from `mpssas_poll` (CAM polled mode for dumps). Apple `IOSCSIParallelInterfaceDevice.cpp:1749` calls `AbortTaskRequest` and the other requests directly, without the command gate.
- We do: we poll the reply queue from the TM caller and the hotplug thread with the reason "the work loop may be gated by our caller", and deliver completions from a 1 s timer.
- Reference does: completions come only from the interrupt path.
- Documented: code comments; design.md "Locking rules" 3.
- Question: the family source shows no gate held on the TM path. Is the pump needed at all? If not, msleep without the pump, as FreeBSD's `mps_wait_command` sleeps. And what lost kick is the 1 s delivery meant to hide? If none, remove it so that a lost kick shows up in tests.
- Prelim: U; risk L (it hides bugs; the extra concurrency is serialized by mps_mtx).
- Status: RESOLVED (KEEP (comment) + CHANGE (lost-kick counter))

Answer: Split. TM-waiter and target-drain pumps: OPEN-NEEDS-HW-TEST, KEEP until then. The pump exists because an OS TM may arrive with our work loop's gate held by the caller (user-client path), in which case the interrupt event source can't run and the waiter would deadlock; the family source we have (`IOSCSIParallelInterfaceDevice.cpp:1749`) shows no gate, but SAM's callers aren't all in src/. Test: log `getWorkLoop()->inGate()` at TM entry and in the drain on hardware through OS aborts/resets; if never in gate -> remove the pump (D10 makes the path rarer anyway). 1 s DeliverCompletions timer: CHANGE -> keep it but count and log (once) every completion it delivers as a "lost kick" statistic, so a missing kick shows up instead of being hidden.

Check: CHALLENGE (reasoning for the pumps; the 1 s timer change is accepted). A hardware test that never sees `inGate()` cannot prove the path never happens. Two facts argue for KEEP (J) with a comment instead of a removal test: (1) FreeBSD itself polls the reply queue while waiting when it may not sleep (`mps_wait_command` NO_SLEEP branch calls `mps_intr_locked`, FreeBSD `mps.c:~3105`), so a pump has FreeBSD precedent; (2) our completions are delivered in-gate from `HandleInterruptRequest` (`kext/SAS2308HBA.cpp:674-680` -> `DeliverCompletions`), so any SAM completion handler that issues a TM synchronously runs with the gate held and would deadlock without the pump. Proposal: KEEP the pumps, comment both reasons, drop the HW-removal plan. With D10 the TM waiter path is rarer anyway.
Answer (round 3): accept. KEEP (J) both pumps with a comment giving both reasons: FreeBSD's own NO_SLEEP `mps_wait_command` polls `mps_intr_locked`, and our completions are delivered in-gate from `HandleInterruptRequest` (`SAS2308HBA.cpp:674-680`), so a completion handler that issues a TM synchronously holds the gate. HW-removal plan dropped; the 1 s timer becomes the counted lost-kick detector as agreed.

Check (round 4): Accepted: both pumps documented with the FreeBSD NO_SLEEP precedent and the in-gate delivery path; the 1 s timer counts lost kicks.

### D44 Timeout completion status: ProtocolTimeoutOccurred, not the family's TaskTimeoutOccurred
_(scan id C14)_
- Ours: `kext/SAS2308HBA.cpp:245-249`
- Reference: Apple `IOSCSIParallelInterfaceController.cpp:1674-1691` (the default HandleTimeout completes `kSCSITaskStatus_TaskTimeoutOccurred`)
- We do: `CAM_CMD_TIMEOUT` → `kSCSITaskStatus_ProtocolTimeoutOccurred`.
- Reference does: TaskTimeoutOccurred for the HBA timer, which is the case here.
- Documented: design.md M5a-3 mapping. FINAL: "no upper layer distinguishes them".
- Question: why not use the family's own status for the family's timer?
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Use kSCSITaskStatus_TaskTimeoutOccurred, the family's own status for its own timer.

Check: Accepted: `kSCSITaskStatus_TaskTimeoutOccurred` as the family's default HandleTimeout (`IOSCSIParallelInterfaceController.cpp:1691`).

### D45 The glue's slot table duplicates the family's task lookup (linear scans of up to 2040 entries under fLock)
_(scan id C15)_
- Ours: `kext/SAS2308Glue.cpp:226-244` (first-free scan per submit), `:310-323` (FindTask), `:341-350` (scan per completion), `kext/SAS2308Glue.h:88-94`
- Reference: Apple `IOSCSIParallelInterfaceController.cpp:1380-1384` (`FindTaskForAddress(T, L, Q)`), `IOSCSIParallelInterfaceDevice.cpp:944` (per device). FreeBSD has no equivalent; the ccb is the state.
- We do: a 32-byte slot per task holds task, prepared, T/L/Q. Every submit and every completion scans it with fLock held.
- Reference does: the family's own outstanding list. `prepared` and the tag could sit in `mps_kext_task` (which already has `os_tag`).
- Documented: design.md "Queue sizes as FreeBSD" ("slot search stays linear").
- Question: why not use FindTaskForAddress for AbortTaskRequest and keep `prepared` in the HBA data, which removes the slots, the slot allocation in Attached, and an O(outstanding) scan at ~12k IOPS?
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (low priority). Use the family's `FindTaskForAddress` for AbortTaskRequest, keep `prepared`/T-L-Q in `mps_kext_task` (HBA task data); removes the slot table and two O(outstanding) scans under fLock per IO. Benchmark before/after at random 4K QD32 (1748 IOPS today).

Check: Accepted (low priority), with the before/after IOPS check.

### D46 Task attribute dropped, and no periodic ordered tag (extends FINAL L2)
_(scan id C16)_
- Ours: `kext/mps_kext.cpp:487` (`MSG_SIMPLE_Q_TAG` always); `kext/SAS2308HBA.cpp:162-183` (GetTaskAttribute not read)
- Reference: CAM `scsi/scsi_da.c:1570-1578,1619-1620,2900` (`da_send_ordered`: an ORDERED tag every timeout/4 against starvation; BIO_ORDERED → ORDERED)
- We do: always SIMPLE.
- Reference does: passes the requested attribute and adds periodic ORDERED tags.
- Documented: no (FINAL L2, still open).
- Question: why not pass `GetTaskAttribute(task)` through (SIMPLE/ORDERED/HEAD) as da passes its tag_action? Is starvation prevention needed with a 30 s timeout and up to 896 commands queued per card?
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE (attribute) / KEEP (no ordered tags))

Answer: agree: CHANGE for the attribute (pass GetTaskAttribute through: SIMPLE/ORDERED/HEAD, as da passes tag_action). KEEP not adding periodic ORDERED tags: measured worst latency at 128 per drive was 1.22 s (bench 2026-11-20), far below the 30 s timeout, and SATA NCQ behind the SATL has no ordered tag (the SATL would drain the queue). Comment that choice.

Check: Accepted: pass the attribute; no periodic ORDERED with the stated reason in a comment.

### D47 ProcessTask refusals before the core differ from FreeBSD
_(scan id C17)_
- Ours: `kext/SAS2308Glue.cpp:169-174` (direction IN/OUT with length 0 → FUNCTION_REJECTED), `:194-222` (IODMACommand prepare/gen failure → synchronous SERVICE_DELIVERY_OR_TARGET_FAILURE, retried at once by SAM)
- Reference: FreeBSD `mps_sas.c:1850-1856` (dxfer_len 0 accepted with any direction, `cm_data = NULL`); busdma EINPROGRESS → SIM frozen and requeued (`mps_sas.c` mapping path), not failed.
- We do: reject or fail synchronously.
- Reference does: accepts zero-length transfers; resource shortages are requeued.
- Documented: no.
- Question: why not treat dir≠NONE with length 0 as NONE, and why not fail a mapping shortage as a requeue (held) instead of an immediate error?
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Direction IN/OUT with length 0 -> treat as NONE (FreeBSD). IODMACommand prepare/gen failure (IOVA space shortage) -> requeue (held, CAM_REQUEUE_REQ) rather than an immediate SERVICE_DELIVERY failure.

Check: Accepted.

### D48 Segment alignment 4 in the constraints, 1 in the DMA spec, 1 in FreeBSD
_(scan id C18)_
- Ours: `kext/SAS2308HBA.cpp:604` (`kIOMinimumSegmentAlignmentByteCountKey` 4) vs `:622-624` (`initWithSpecification(..., alignment 1, ...)`)
- Reference: FreeBSD `mps_pci.c` parent tag and `mps.c:1494-1500` buffer_dmat (alignment 1, maxsegsize 24-bit)
- We do: tell the storage stack that buffers must be 4-byte aligned while the DMA command accepts any alignment.
- Reference does: alignment 1. MPI2 64-bit simple SGEs take byte addresses.
- Documented: no.
- Question: why 4? It can make IOBlockStorageDriver bounce unaligned pass-through buffers for no reason. Use 1, consistent with the DMA spec.
- Prelim: U; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE. Alignment 1 in the constraints, consistent with the DMA spec and FreeBSD; MPI2 64-bit simple SGEs take byte addresses.

Check: Accepted.

### D49 Sleep: Linux-style quiesce/MUR/diag-reset, synchronous inside setPowerState; FreeBSD refuses suspend
_(scan id C20)_
- Ours: `kext/SAS2308HBA_PM.cpp:95-112,141-185`, `kext/SAS2308HBA.h:74`
- Reference: FreeBSD `mps_pci.c:389-399` (`mps_pci_suspend`/`resume` return EINVAL, which vetoes suspend); Linux `scsih_suspend`/`resume` (`scratchpad/src/scsih.c`)
- We do: drain up to 10 s, then MUR, all inside setPowerState, returning kIOPMAckImplied. Doze maps to off. `fSuspended` is set even when the MUR failed (`:154-156`). A failed resume is logged at INFO ("controller not running", `:183-184`).
- Reference does: FreeBSD has no sleep support. Linux quiesces the midlayer and does a hard reset on resume.
- Documented: design.md "Power management (sleep / wake)".
- Question: (a) why not ack asynchronously (return a max wait and `acknowledgeSetPowerState`) instead of blocking the PM thread up to 10 s + MUR? (b) why is a resume that could not run logged at INFO and not FAULT?
- Prelim: K for the design (it has to deviate: there is no FreeBSD model); J for (a) and (b); risk L.
- Status: RESOLVED (KEEP (a, comment) / CHANGE (b))

Answer: (a) KEEP synchronous, with the reason in a comment: measured drain + MUR + reinit well under a second (498 ms wake, measured on the test setup), and the bounded 10 s worst case is inside the PM thread's tolerance; an async ack adds a second state machine for no measured benefit. (b) agree: CHANGE: a resume that could not run is a FAULT line; and don't set `fSuspended` when the MUR failed (`SAS2308HBA_PM.cpp:154-156`) without saying so (log + statistic).

Check: Accepted.

### D50 Shutdown: IOC stopped with a MUR at systemWillShutdown; FreeBSD only does IR shutdown and SSU
_(scan id C21)_
- Ours: `kext/SAS2308HBA_PM.cpp:120-134`, `kext/SAS2308HBA.h:75`
- Reference: FreeBSD `mps.c:2059-2062` (shutdown_final → `mpssas_ir_shutdown`), `mps_sas_lsi.c:1225-1330` (IR action plus `mpssas_SSU_to_SATA_devices`; default `enable_ssu` = SSD only, `mps.c:1581`). FreeBSD never stops the IOC.
- We do: a 1 s drain, a MUR, then all new IO fails DEV_NOT_THERE. SSU is not ported.
- Reference does: the IOC keeps running; START STOP UNIT goes to SATA SSDs.
- Documented: design.md "Shutdown / restart" (the dock has its own PSU, DMA into the next kernel).
- Question: on Apple silicon the Thunderbolt link (and so the tunnel and the DART) is reset at restart. Is the MUR still needed, or is it a precaution? If kept, OK. Port `mpssas_SSU_to_SATA_devices` for SATA SSDs to keep FreeBSD's default.
- Prelim: J; risk L.
- Note: the SSU part is in the D9 entry; this entry is about the MUR at shutdown.
- Status: RESOLVED (KEEP (comment))

Answer: KEEP the MUR (precaution, cheap). On restart the dock keeps power (own PSU) while the Mac resets the Thunderbolt tunnel and the DART; a still-running IOC would keep DMAing into addresses the next boot doesn't map (DART faults, or worse if a mapping is reused). The MUR stops it in < 1 s. Comment to that effect in `SAS2308HBA_PM.cpp`. SSU: see D9.

Check: Accepted: the stated reason (dock PSU keeps the IOC running across the Mac's restart; DART/IOVA not valid for the next boot) is accurate and belongs in `SAS2308HBA_PM.cpp`.

### D51 StopController disables the interrupt before mps_free (FreeBSD: mps_free, then the interrupt teardown)
_(scan id C22)_
- Ours: `kext/SAS2308HBA.cpp:438-439`
- Reference: FreeBSD `mps_pci.c:340-343` (`mps_free(sc)` then `mps_pci_free(sc)`, which tears down the interrupts)
- We do: `DisableInterrupt()` then `fGlue.StopCore()` (mps_free).
- Reference does: interrupts stay live while mps_free stops the IOC and completes commands.
- Documented: no.
- Question: what does the reversed order buy? Anything mps_free waits for that completes through the reply queue (a config request, the REMOVE_DEVICE of a draining target) now depends on M2's ENXIO short-cut. Keep FreeBSD's order unless there is a family reason.
- Prelim: Q; risk L.
- Status: RESOLVED (KEEP (order, docs) + CHANGE (polled drain))

Answer: KEEP the order (J), CHANGE the docs, plus a host test. The family owns the interrupt source; DisableInterrupt is our only way to guarantee no `mps_intr` runs while `mps_free` frees the reply/post queues. FreeBSD can tear down after mps_free because bus_teardown_intr waits for a running handler; we have no equivalent ordering after the queues are gone. What mps_free waits for completes without interrupts: config waits use the M2 ENXIO short-cut and TMs are completed by `mpssas_complete_all_commands`. Host test: unload with an outstanding config request and a draining removal -> mps_free returns, nothing waits on a reply.

Check: CHALLENGE (one gap). The order is justified (no `mps_intr` may run while `mps_free` frees the queues; FreeBSD gets that from `bus_teardown_intr`). But with interrupts off, `mps_free` (`core/mps.c:2141-2160`) MURs the IOC and then fails everything (`mpssas_xnu_fail_all`) without draining the reply queue first: IO the IOC already completed (reply posted, interrupt not yet delivered) is reported DEV_NOT_THERE instead of its real status. FreeBSD's interrupt is still live at that point, so those complete normally. Fix inside the core, no reorder: one polled `mps_intr_locked` after setting SHUTDOWN and before the MUR (FreeBSD's own NO_SLEEP polling pattern). Host test: reply posted, no interrupt, unload -> the ccb completes CAM_REQ_CMP. Plus your docs + host test.
Answer (round 3): accept. Verified `mps_free` (`core/mps.c:2141-2160`) MURs then `mpssas_xnu_fail_all` with interrupts already off. Final fix: one polled `mps_intr_locked` after setting SHUTDOWN and before the MUR (FreeBSD's NO_SLEEP polling pattern), order unchanged; docs + host tests (reply posted with no interrupt, unload -> CAM_REQ_CMP; outstanding config request + draining removal -> mps_free returns).

Check (round 4): Fix verified: `mps_intr_locked` processes replies with SHUTDOWN set (it returns early only on DIAGRESET/GONE, `core/mps.c:2386-2389`), and new IO is already failed under SHUTDOWN, so one poll before the MUR completes finished IO with its real status.

### D52 Personality matches only 0x0087
_(scan id C24)_
- Ours: `kext/Info.plist:37-44`
- Reference: FreeBSD `mps_pci.c:123-137` (SAS2308_1 0x0086, SAS2308_2 0x0087, SAS2308_3 0x006E, plus the SAS2004/2008/2108/2116/2208 family)
- We do: one device ID, IOProbeScore 1000.
- Reference does: the whole MPI2 family.
- Documented: design.md header (target card).
- Question: should at least 0x0086/0x006E (other SAS2308 SKUs, same firmware interface) match, now that the core is FreeBSD's?
- Prelim: Q; risk L.
- Status: RESOLVED (CHANGE)

Answer: agree: CHANGE (SAS2308 SKUs only). Add 0x0086 and 0x006E, same chip and firmware interface; leave the other MPI2 chips out until tested.

Check: Accepted: 0x0086 and 0x006E only.

## Batch 2: deviations the scan would keep (K): confirm, or object in one line

### D53 Several FreeBSD features removed outright: TLR, WarpDrive, IR/volume events, enclosure mapping, SMP passthrough, XPT_PATH_INQ/GET_TRAN_SETTINGS/CALC_GEOMETRY
_(scan id A13)_
- Ours: `core/mps_sas.c:1628,1950,1990` (TLR), `:1671,1920` (DD IO), `:719-721` (XPT_*), `core/mps_sas_lsi.c:128,261-264` (IR, ENCL); `mpssas_register_events` still sets the IR_* and ENCL bits
- Reference: `mps_sas.c:1735,2121-2131,2215-2243` (TLR only for SSP sequential-access devices), `mps_sas_lsi.c:219-500`
- We do: no TLR bits (= NO_TLR, which is what FreeBSD also sends to disks). IR and enclosure events are still registered, then logged and ignored.
- Reference does: TLR for SAS tape; IR handling on IR firmware.
- Documented: design.md M5a-3 "Not ported"
- Question: why register IR and ENCL events we then drop? Unmask only what is handled, or keep the handlers verbatim; they are harmless on IT firmware. Should a SAS tape behind this HBA get TLR?
- Prelim: K (IT firmware, disks); risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm). Keeping FreeBSD's event registration is closer to the reference than trimming the mask; the IR/ENCL events are logged and ignored on IT firmware. No TLR: no SAS tape on this HBA; FreeBSD also sends NO_TLR to disks.

Check: Accepted (FreeBSD's registration kept; NO_TLR as FreeBSD for disks).

### D54 Reinit runs on a taskqueue; requests are coalesced and IO is held while one is pending
_(scan id A17)_
- Ours: `core/mps.c:991-1004` (`mps_reinit_task`), `core/mps_xnu.c:624-647`, `core/mps.c:870-886`, `core/mps.c:2050-2056`, `core/mps_sas.c:874-897` (`mpssas_tm_timeout`), `:1552-1555`
- Reference: `mps.c:808-855` (`mps_reinit` called inline from the callout `mps_periodic`, from `mpssas_tm_timeout` and from the NULL-reply paths)
- We do: queue a task. New IO is requeued and OS TMs get BUSY while `reinit_pending`. A timed-out TM stays owned (late reply counted) until the reinit frees it.
- Reference does: resets inside the callout. `mps_diag_reset(CAN_SLEEP)` sleeps there, which is invalid in callout context on FreeBSD as well.
- Documented: design.md "Rules for the port", M5a-2, "Review fixes" H2
- Question: none on the principle. Could the reinit task live in compat, so that `mps_periodic` and `mpssas_tm_timeout` keep FreeBSD's literal `mps_reinit(sc)` call through a compat wrapper?
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP the principle. CHANGE (cosmetic, low priority): a compat `mps_reinit()` wrapper that queues the task, so `mps_periodic`/`mpssas_tm_timeout`/NULL-reply call sites stay literally FreeBSD's.

Check: Accepted, incl. the compat `mps_reinit()` wrapper.

### D55 No panic: failed reinit, failed allocation or bad reply leads to a disabled controller and logs
_(scan id A18)_
- Ours: `core/mps.c:489-497` (IOCFacts), `:937-971` (3 attempts then `mps_xnu_disable`), `:1786-1795` (command dmamap), `:2437-2463` (reply address out of range, dropped)
- Reference: `mps.c:856` (allocation), `:1531` (command), `:2454` (reply address) (panic)
- We do: log, count, disable or drop.
- Reference does: panics the machine.
- Documented: design.md "Rules for the port", M5a-3 "No panic"
- Question: `MPS_XNU_REINIT_TRIES = 3` is a guess. FreeBSD tries once, Linux retries a diag reset up to 3 times in `_base_diag_reset` paths. Fine.
- Prelim: K; risk L.
- Also (scan id B20, merged here): KASSERT/MPASS are always evaluated, log two lines and continue; a recursive `mtx_lock` still panics inside lck_mtx, so "never panic" is not absolute. Count KASSERT hits in the statistics so a hardware run can be checked for them?
- Status: RESOLVED (KEEP)

Answer: KEEP. Agree to add a "KASSERT hits" counter to SAS2308Statistics so hardware runs can be checked for them; MPS_XNU_REINIT_TRIES=3 matches Linux.

Check: Accepted, plus the KASSERT-hits counter.

### D56 Reply-descriptor hardening (SMID range, idle SMID, frame alignment, late TM reply)
_(scan id A19)_
- Ours: `core/mps.c:2338-2366`, `:2428-2431`, `:2451-2464`, `:2498-2500`
- Reference: `mps.c:2412-2500` (no SMID check on SCSI_IO_SUCCESS; panic on an address out of range; INQUEUE check only on address replies)
- We do: drop and count; give the reply frame back.
- Reference does: completes a stale SCSI_IO_SUCCESS SMID, or panics.
- Documented: design.md M5a-3 "Robustness"
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D57 mps_intr_locked returns at once while DIAGRESET or GONE
_(scan id A20)_
- Ours: `core/mps.c:2386-2389`
- Reference: `mps.c:2367-2380` (no such check; interrupts are masked during reinit)
- We do: polled callers (TM waiters, `mps_xnu_ccb_timeout`, the suspend drain) cannot walk a post queue that `mps_iocfacts_allocate` is reallocating.
- Reference does: relies on the mask, and on everything running under one lock with no other polling callers.
- Documented: code comment
- Question: none; the extra polling callers make it necessary.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D58 Queue sizing: post queue and num_replies, plus an 8-command internal reserve for the OS
_(scan id A21)_
- Ours: `core/mps.c:369-418` (`MaxReplyDescriptorPostQueueDepth - prireqcr - 1`, `num_replies = MIN(MAX(max_replyframes, num_reqs) + max_evtframes, depth) - 1`, `xnu.io_cmds = reqcr - MPS_XNU_INTERNAL_CMDS`), `core/mps_xnu.h:30`; `core/mps_sas.c:560` (SIM depth is still FreeBSD's `num_reqs - num_prireqs - 1`)
- Reference: `mps.c:379-398`, `mps_sas.c:720`
- We do: the post queue holds a descriptor for every request frame. The family gets 8 fewer tasks than there are frames.
- Reference does: 2080-entry post queue for 2175 frames; CAM gets every normal frame.
- Documented: design.md "Queue sizes as FreeBSD", code comments
- Question: none on the post queue (Linux sizes it as queue depth + reply free depth + 1). The reserve of 8 is arbitrary, but at 2040 tasks it costs nothing.
- Prelim: K; risk L.
- Note: the 8-command reserve is questioned separately (D21); this entry is about the post-queue sizing only.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm); the reserve itself is answered in D21.

Check: Confirmed; reserve handled in D21.

### D59 Sizing guards: request frame ≥ 128 bytes, segments ≤ chain depth, num_chains ≥ 1
_(scan id A23)_
- Ours: `core/mps.c:500-515`, `:1738-1747`, `:438-439`
- Reference: `mps.c:1494` (nsegs = maxio/PAGE_SIZE + 1)
- We do: refuse firmware with small frames; cap nsegs at `(frame/12 - 1) * MaxChainDepth + 1`; never allocate 0 chains.
- Reference does: no guards.
- Documented: design.md M5a-2
- Question: none; no effect on the SAS2308 (128-byte frames, depth 128).
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D60 INDIAGRESET IO requeued only while IN_STARTUP; other XNU checks in mpssas_action_scsiio
_(scan id A24)_
- Ours: `core/mps_sas.c:1457-1493` (GONE or shutdown fails the IO, TID bound instead of KASSERT, INDIAGRESET requeue only during startup), `:1637-1644` (cdb_len), `:1686-1706` (EFBIG/EINVAL undo)
- Reference: `mps_sas.c:1630-1650` (INDIAGRESET requeue at :1637)
- We do: after the rediscovery, a target that did not come back fails `CAM_DEV_NOT_THERE` instead of requeueing.
- Reference does: requeue with devq frozen. FreeBSD's periph releases later and the lost-device async cleans up.
- Documented: code comments; design.md M5a-2
- Question: none; it depends on the thin CAM releasing the freeze at once.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D61 One MSI vector, no MSI-X
_(scan id A27)_
- Ours: `core/mps.c:1851`
- Reference: `mps_pci.c` (MSI-X first, then MSI); `mps.c:433-447`
- We do: `msi_msgs = 1`, MSI only.
- Reference does: MSI-X if available, but mps uses one reply queue anyway.
- Documented: code comment; design.md "Queue sizes" (MaxMSIxVectors 0 on the 9207 firmware)
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm): MaxMSIxVectors reads 0 on this firmware (Controller Limits, 2026-11-20).

Check: Confirmed: MaxMSIxVectors 0 in Controller Limits.

### D62 Removal of FreeBSD's mapping layer: id policy and no persistence
_(scan id A29)_
- Ours: `core/mps_xnu.c:434-497` (the id that last had this identity; else a never-used id; else the first free one); `core/mps.c:964-971` (no IOC Page 8, no `mps_mapping_initialize`)
- Reference: `mps_mapping.c` (DPM / enclosure-slot persistent mapping keyed on SAS address, IOC Page 8 mapping mode), `mps_config.c:355-370`
- We do: ids are in discovery order at every boot. A disk swapped into a bay gets a new id (never-used first), not the bay's id.
- Reference does: ids are persistent across boots, and slot-stable in enclosure mode.
- Documented: design.md "Stubbed / deferred", code comment
- Question: is anything on macOS keyed on the target id (ioreg "SCSI Target Identifier", user scripts)? If not, fine. If yes, why not port the enclosure/slot mapping, which needs only SAS Enclosure Page 0 and the event handlers already present upstream?
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP. Nothing on macOS is keyed on the target id: volumes mount by UUID, disks are matched by serial/WWN (C2 fix). Revisit with an enclosure/expander (slot-stable ids would help there).

Check: Accepted: no macOS consumer keyed on the target id; revisit with an expander.

### D63 MPS_XNU_RESETTING widens "NULL reply = reset" to GONE and SHUTDOWN
_(scan id A31)_
- Ours: `core/mpsvar.h:313-320`; used at `core/mps_sas.c:936,1035,1177,2194,2356,2404`, `:1874-1881`
- Reference: `mps_sas.c:1150,1250,1385` (`MPS_FLAGS_DIAGRESET` only)
- We do: TM and IO completions without a reply during detach or unplug are cleanup, not "request a reinit". IO gets `CAM_DEV_NOT_THERE`.
- Reference does: would call `mps_reinit` from the detach path.
- Documented: design.md M5a-2
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D64 mps_attach and mps_free ownership; event registration checked; dropped IOMalloc checks
_(scan id A32)_
- Ours: `core/mps.c:1947-1985`, `:2099-2101`, `core/mps_sas.c:526-528,541-556,582-590,617`, `core/mps.c:2741-2742`
- Reference: `mps.c:2002` `mps_attach` (`mps_free` in the caller and twice inside `mps_iocfacts_allocate`), `mps_sas.c:803` (registration result ignored)
- We do: free exactly once from `mps_attach`; a failed event registration fails the attach.
- Reference does: double free on some paths (FINAL M3, fixed).
- Documented: design.md "Review fixes" M3
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D65 Thin CAM ignores openings (devq max_openings, max_dev/max_tagged_dev_openings)
_(scan id B6)_
- Ours: `core/compat/cam_compat.h:248-250, 279-281`, `core/compat/cam_compat.c:17-26, 34-59` (stored, never read); `core/mps_sas.c:561-569`
- Reference: `cam/cam_xpt.c:4962-4964, 4990` (device openings = min(maxtags, sim max_tagged_dev_openings)), xpt_run_devq stops at send_openings == 0; periph shrinks openings on QUEUE FULL (`xpt_dev_ccbq_resize`)
- We do: every XPT_SCSI_IO goes straight to `mpssas_action`; the only throttle is the family's ReportMaximumTaskCount = `xnu.io_cmds`; a cm==NULL miss uses the FreeBSD requeue+simq-freeze fallback.
- Reference does: CAM never offers more than `reqs` ccbs; per-device depth bounded and adapted.
- Documented: design.md "Queue sizes as FreeBSD" (family pool = io_cmds); TASK SET FULL handled by the family's resend list (`src/IOSCSIParallelInterfaceDevice.cpp:1393-1401`, FINAL section 3).
- Question: none functional; suggest a comment in cam_compat.h that openings are deliberately not enforced (family does it), so nobody relies on them.
- Prelim: K; risk L; equivalent throttling exists one layer up.
- Status: RESOLVED (KEEP)

Answer: KEEP; agree to add the comment in cam_compat.h: "openings are stored but not enforced; the family's ReportMaximumTaskCount (= io_cmds) is the throttle".

Check: Accepted with the cam_compat.h comment.

### D66 Held IO keeps its original arrival order on requeue
_(scan id B7)_
- Ours: `core/compat/cam_compat.c:216-236, 283-286, 410-411`
- Reference: `cam/cam_xpt.c:3427` (generation assigned once in xpt_setup_ccb), `cam/cam_queue.c:293-302` (heap ordered by priority, then generation), `cam/cam_periph.c:2111-2115` (retry re-enters with the old generation)
- We do: `xnu_seq` assigned at the first xpt_action; requeued ccbs are re-inserted by it.
- Reference does: same effect (a retried ccb goes back in front of younger ones of the same priority).
- Documented: header `cam_compat.h:14-16`; design.md contract row.
- Question: none; priority is ignored (all CAM_PRIORITY_NORMAL), fine.
- Prelim: K; risk L; faithful.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D67 CAM_REQUEUE_REQ is retried without bound and without a "periph invalid" check
_(scan id B9)_
- Ours: `core/compat/cam_compat.c:410-411`
- Reference: `cam/cam_periph.c:1984-1995` (ERESTART unless CAM_PERIPH_INVALID or SF_NO_RETRY)
- We do: every REQUEUE is held again; only the M6 reset-kill path is bounded (`ccb_h.xnu_resets`, core); a target being destroyed is handled by `cam_xnu_flush_held_target` (M7).
- Reference does: unbounded too, but stops when the periph is invalidated.
- Documented: design.md "Review fixes" M6, M7.
- Question: the invariant "every REQUEUE is backed by a freeze (simq or devq) or it either livelocks in `run_held` (QFRZN without a persistent freeze sets rerun) or strands until the next xpt_action" is only implicit; why not assert it in xpt_done (REQUEUE && !blocked && run) to catch a future core change?
- Prelim: K (add assert); risk L; correct today (all REQUEUE paths in `core/mps_sas.c:1484, 1543, 1563, 1848, 1880` are backed by a freeze).
- Status: RESOLVED (KEEP)

Answer: KEEP; agree to add the assert in xpt_done (REQUEUE must be backed by a simq or devq freeze).

Check: Accepted with the xpt_done assert.

### D68 Rescan model: AC_XNU_SCAN + present sampling replaces CAM's probe
_(scan id B12)_
- Ours: `core/compat/cam_compat.c:508-520`, `core/compat/cam_compat.h:131-132`
- Reference: `cam/cam_xpt.c:832-877` + scsi_xpt probe (INQUIRY, REPORT LUNS; AC_FOUND_DEVICE / AC_LOST_DEVICE from the probe result)
- We do: XPT_SCAN_TGT/BUS is turned into an async event; presence = `mps_xnu_target_present`; the family probes LUNs itself.
- Reference does: the XPT probes the target and derives found/lost.
- Documented: design.md M5a-3 "Hotplug".
- Question: none beyond D7; reasonable mapping onto Create/DestroyTargetForID.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm); target count in D7.

Check: Confirmed.

### D69 xpt_hold_boot / xpt_release_boot are no-ops
_(scan id B15)_
- Ours: `core/compat/cam_compat.h:336-337`
- Reference: `cam/cam_xpt.c:874` (rescans hold boot), mps_sas.c startup increment/decrement
- We do: nothing.
- Reference does: delays mountroot until discovery finishes.
- Documented: no.
- Question: none; the kext loads after boot and the HBA is not a boot device. One-line comment would do.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP; add the one-line comment (kext loads after boot, HBA is not a boot device).

Check: Accepted with the comment.

### D70 msleep/wakeup mapping
_(scan id B22)_
- Ours: `core/compat/mps_compat.c:500-529`, `kext/mps_xnu_os.cpp:259-279`
- Reference: msleep(9) without PCATCH (uninterruptible), returns EWOULDBLOCK on timeout; wakeup(9) wakes all sleepers on chan
- We do: IOLockSleep/IOLockSleepDeadline THREAD_UNINT on chan, owner bookkeeping around it; wakeup = IOLockWakeup(gWakeLock, chan, all) (lock argument unused by xnu); wakeups before `mps_kext_os_init` are dropped.
- Reference does: same semantics.
- Documented: design.md contract row.
- Question: none.
- Prelim: K; risk L; faithful.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D71 Callouts: one thread_call per mutex, armed for the earliest deadline, +1 us
_(scan id B23)_
- Ours: `core/compat/mps_compat.c:559-684`, `kext/mps_xnu_os.cpp:178-198, 469-489`
- Reference: callout(9) with callout_init_mtx: handler runs with the mtx, callout_stop under the mtx guarantees no run; callout_drain waits for a running handler
- We do: sorted list per mtx; `mps_compat_callout_run` under the mtx; `thread_call_enter_delayed((deadline_us+1) us)`; drain == stop (FINAL L11).
- Reference does: as above; precision flags (C_PREL) honoured.
- Documented: design.md contract row (+1 us, inflight counting, cancel_wait private).
- Question: thread_call_enter_delayed without THREAD_CALL_DELAY_LEEWAY still gets system timer coalescing; fine for 1 s/5 s/30 s timers. Was `thread_call_allocate_with_options(..., THREAD_CALL_OPTIONS_ONCE)` + `thread_call_cancel_wait` checked against the public Kernel.framework headers (removes the inflight/WaitIdle polling)?
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP. `thread_call_cancel_wait` is `kpi.private` (design.md:74), so the inflight counting stays; THREAD_CALL_OPTIONS_ONCE alone doesn't replace it. Timer coalescing is fine for 1/5/30 s timers.

Check: Accepted (`thread_call_cancel_wait` is kpi.private, design.md contract table).

### D72 bus_dmamap_load_ccb refuses instead of splitting
_(scan id B28)_
- Ours: `core/compat/cam_compat.c:566-604`
- Reference: `_bus_dmamap_load_ccb` / plist load: splits >maxsegsz, coalesces, EFBIG via callback when out of segments
- We do: refuse (no callback) a list that is not CAM_DATA_SG_PADDR, has 0-length or >maxsegsz segments, more than nsegments, or does not add up to dxfer_len.
- Reference does: builds a legal list.
- Documented: design.md M5a-2 bullet; code comment.
- Question: none; the kext builds the list with the same constraints (IODMACommand maxSegmentSize).
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D73 bus_dmamap_sync is a no-op
_(scan id B29)_
- Ours: `core/compat/mps_compat.h:550-557`
- Reference: FreeBSD mps syncs only the IO buffer map (`mps/mps.c:3018, 3233`, `mps/mps_sas.c:1991, 2719`), never the queues
- We do: nothing; ordering is provided by the H3 barriers in the MMIO hooks.
- Reference does: bounce/cache maintenance where the platform needs it.
- Documented: design.md contract row ("DART PCIe DMA is coherent"); FINAL L17 (bounced IODMACommand).
- Question: none for Apple silicon (IO-coherent PCIe). The family's IO buffers go through the kext's own IODMACommand; if that one could ever bounce, the no-op sync is wrong; assert "no bounce" there.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP; agree to assert "no bounce" in the kext's IODMACommand path (FINAL L17).

Check: Accepted with the no-bounce assert (note D6 alternative (1) would make sync a copy for bounced maps).

### D74 DMA memory: IOBufferMemoryDescriptor + IODMACommand(kMapped, 32/64 bits, mapper)
_(scan id B30)_
- Ours: `kext/mps_xnu_os.cpp:340-461`
- Reference: bus_dmamem_alloc + bus_dmamap_load (`mps/mps.c` tag setup, 32-bit lowaddr for queues/frames/chains/sense)
- We do: IOVA below 4 GiB by IODMACommand numAddressBits 32 on the device mapper (DART); without a mapper physMask + physically contiguous for 1-segment tags; segment list computed once; unload keeps the IOVA mapped until free; buffer zeroed twice (`:414` and `mps_compat.c:984-985`).
- Reference does: physical addresses below lowaddr.
- Documented: design.md contract row; hardware verified (M5b).
- Question: none functional. The kext validates alignment by hand although IODMACommand takes an alignment argument (passed 1); pass `dt_alignment` to withSpecification instead?
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP; agree (minor): pass `dt_alignment` to `withSpecification` instead of checking by hand, and drop the double zeroing.

Check: Accepted with the two minor changes.

### D75 MMIO hooks: barriers, all-ones when unmapped, default mapping type
_(scan id B31)_
- Ours: `kext/mps_xnu_os.cpp:307-338`; mapping `kext/SAS2308HBA.cpp:321`
- Reference: bus_space_read_4/write_4 (x86 ordering); Linux readl/writel (dma_rmb/wmb)
- We do: `dmb oshst` before each store, `dmb oshld` after each load (arm64e only, H3); h == 0 reads all-ones, writes dropped; BAR1 mapped with default options.
- Reference does: plain accesses on x86.
- Documented: code comment (Review H3), design.md Review fixes H3.
- Question: the default device mapping on Apple silicon is non-posted (nGnRnE); was kIOMapPostedWrite considered for the doorbell/host-index writes (Apple recommends it for PCIe BARs; ordering is already handled by the barriers)?
- Prelim: K (barriers), Q (mapping type); risk L.
- Status: RESOLVED (KEEP (barriers) + HW-TEST (mapping type))

Answer: KEEP the barriers. Mapping type: OPEN-NEEDS-HW-TEST: map BAR1 with kIOMapPostedWrite (ordering already by the H3 barriers) and compare random 4K QD32 IOPS and the reply-post host-index write rate vs today; keep whichever is faster with 0 errors.

Check: Accepted: kIOMapPostedWrite A/B with IOPS and 0 errors.

### D76 Interrupt hooks are no-ops; the family owns the event source
_(scan id B32)_
- Ours: `kext/mps_xnu_os.cpp:550-561` -> `kext/SAS2308HBA.cpp:61-75`
- Reference: `mps/mps_pci.c` mps_pci_setup_interrupts (MSI/INTx alloc, bus_setup_intr), free on detach/reinit
- We do: setup/free return immediately; interrupts come from IOFilterInterruptEventSource created by IOSCSIParallelFamily, enabled in StartController; `mp_ncpus` 1 keeps a single vector (`core/mps.c:450`).
- Reference does: allocates/tears down its own vectors.
- Documented: design.md contract row.
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D77 Time base: hz 1000, ticks/sbinuptime from mach_absolute_time
_(scan id B33)_
- Ours: `core/compat/mps_compat.h:308-331`, `core/compat/mps_compat.c:378-441`, `kext/mps_xnu_os.cpp:291-305`
- Reference: hz 1000, ticks (int, wraps), sbinuptime (no suspend time), getmicrouptime (tick-cached)
- We do: same units; uptime excludes system sleep like FreeBSD; getmicrotime = calendar time (IOCInit TimeStamp).
- Reference does: same.
- Documented: design.md contract row.
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D78 queue.h subset without QUEUE_MACRO_DEBUG
_(scan id B34)_
- Ours: `core/compat/queue.h:36-51`
- Reference: FreeBSD sys/sys/queue.h (QMD_* checks, TRASHIT)
- We do: SLIST/TAILQ only, all debug macros empty.
- Reference does: optional trash/consistency checks.
- Documented: header comment.
- Question: enable QUEUE_MACRO_DEBUG_TRASH (and the QMD_TAILQ checks) in the host test build; it would have caught list misuse like FINAL L3 (`cam_xnu_cancel_held` on a ccb in the run_held batch) mechanically.
- Prelim: K (+ test suggestion); risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP; agree to enable QUEUE_MACRO_DEBUG_TRASH and the QMD_TAILQ checks in the host test build.

Check: Accepted with QUEUE_MACRO_DEBUG in host tests.

### D79 Capabilities: as FreeBSD XPT_PATH_INQ (keep)
_(scan id C25)_
- Ours: `kext/SAS2308HBA.cpp:539-578` (initiator ID = max targets, max LUN 255, DoesHBAPerformDeviceManagement, autosense, no parallel features), `:589-625` (segment count from buffer_dmat, segment bytes 0xFFFFFF page-aligned)
- Reference: FreeBSD `mps_sas.c:916-944` (`initiator_id = maxtargets`, `max_lun = 255`, `PIM_NOSCAN|PIM_NOBUSRESET`), `mps.c` buffer_dmat (maxsegsize 24-bit)
- We do / Reference does: the same, except the target count (D7) and the alignment (D48).
- Documented: code comments.
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm); D7/D48 changes carry over.

Check: Confirmed.

### D80 TerminateTargets destroys ID 63, which the family's willTerminate loop misses (keep)
_(scan id C26)_
- Ours: `kext/SAS2308Glue.cpp:559-575`, `kext/SAS2308HBA_Targets.h` (`Leftovers`)
- Reference: Apple `IOSCSIParallelInterfaceController.cpp:522` (`index < fHighestSupportedDeviceID`; create uses `<=`, `:369`)
- We do: destroy the leftover.
- Reference does: the family bug.
- Documented: yes (header comment, design.md "Termination").
- Question: none.
- Prelim: K; risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP (confirm).

Check: Confirmed.

### D81 Properties and statistics (observability only; keep)
_(scan id C27)_
- Ours: `kext/SAS2308HBA_Props.cpp` (Controller Characteristics with a product guessed from the subsystem ID and "6 Gigabit" fixed; Controller Limits; SAS2308Statistics; IOC Temperature, polled every 30 s by the core through IO Unit Page 7)
- Reference: FreeBSD sysctl tree (`dev.mps.N.*`); no temperature poll.
- We do: registry equivalents. The temperature poll is a new periodic firmware config request (core).
- Documented: design.md "Properties published", "IOC temperature".
- Question: should the 30 s IO Unit Page 7 poll be off by default (FreeBSD sends no periodic config requests), given that each one uses a command and a firmware event slot?
- Prelim: K (properties); Q (poll); risk L.
- Status: RESOLVED (KEEP)

Answer: KEEP the properties; poll stays on by default (user-requested temperature readout) with D31's safer failure mode.

Check: Accepted for the properties; the poll default depends on D31.

