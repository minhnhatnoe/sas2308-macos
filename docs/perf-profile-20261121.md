# SAS2308HBA host-side cost per 4 KiB IO: profile of 085DA9D6 (2026-11-21)

Question: why the host-bound 4 KiB point dropped from 170.3k IOPS (2026-11-20, build 67BF18CF) to
157.6k (final run, 085DA9D6 = master fae7e39), i.e. from ~5.8 to ~6.3 us per IO at the serial
bottleneck. Kext 085DA9D6 loaded throughout (same boot as the final run, up since ~23:25 on 11-20),
log level 2, debug mask 35, 7 x ST18000NM000J (disk4-10, serials as in the final run), raw reads only.
No driver change, no install, no reboot.

## Result in short

- **Reproduced:** F1 4 KiB point (sasbench `-b 4K -q 2`, 7 drives, outer zone), 3 x 30 s:
  **161,940 / 160,914 / 160,877 IOPS** (mean 161.2k, p50 80 us, p99 144 us), 9 runs today in
  160.2k-161.9k. That is -5.3 % against 170.3k, +2.3 % against the final run's 157.6k (same build,
  same boot: run-to-run spread within a boot is about 2 %).
- **The bottleneck is the controller's work-loop gate, not CPU.** All completions run in the one
  work-loop thread with the gate held, and every submit takes the same gate three times. The
  work-loop thread is on CPU **4.41 us per IO**, 69 % of a Super core, and holds the gate for 3.2 us of
  that. The 14 submitting threads spend **6.9 us of CPU per IO spinning for the gate**. Of the work
  loop's time, **1.80 us/IO (41 %) is the DART unmap** (`IODMACommand::complete` -> IODARTMapper ->
  SPTM) that our `DeliverCompletions` calls before `CompleteParallelTask`. SAM takes 0.58 us,
  IOStorage 0.51 and our kext 0.17-0.20.
- **Our kext's own code costs 0.33 us per IO in all** (PC samples in the kext, submit and completion
  together). The batches A-F add at most ~0.1 us of that, ≤0.05 us inside the gate (static diff of the
  two binaries' hot paths, below). Log checks (batch E) measure as no change: debug mask 0 / 35 / 631
  gives 160.4k / 160.9k / 160.3k. The family's task pool is LIFO (14 distinct tasks in 10 s), so 2040
  tasks cost nothing there.
- **Attribution:** batches A-F do not explain a 0.3-0.5 us/IO regression. Confidence is high for ≥0.15
  us/IO coming from kext code, low for what the rest is. The A/B against EF9A8035 was **not run**: the
  install needs the approval in System Settings, and no approval client (keyboard or Screen
  Sharing) was available. The open explanations are the baseline itself (another build, 67BF18CF with 64
  commands, and another boot), the environment (audiomxd at 72 % and configd at 42 % CPU since this
  boot), and cost outside the kext.
- **Largest lever (new):** take the DART unmap out of the gated completion path, expected gain
  +15-30 % at this point. Next, drop the per-IO `SetTimeoutForTask` gate round trip, +5-10 %.

## Method

Scripts in `bench/profile/scripts/` (run with sudo; output in `bench/out/profile/<TAG>`, or `OUT=`):

| script | what |
|---|---|
| `repro.sh` | F1 4 KiB point 3 x 30 s, then 1 drive QD2, 1 drive QD1, 7 drives QD1 (30 s each); `sas2308ctl stats` before/after each |
| `base.sh` | load address of the kext: callers of `CompleteParallelTask` / `SCSIParallelTimer::SetTimeout` (return sites in `DeliverCompletions` 0x141dc and `ProcessTask` 0x14870) -> **base 0xfffffe004394c000** this boot. `kmutil showloaded` prints address 0 for the kext and fbt has no probes in it. |
| `profile.sh` | one 200 s F1 load with 20 s DTrace windows: `prof.d` (profile-4997, kernel `stack()`, kext PCs), `sched.d`, `t1.d`, `t2.d`, `locks.d` |
| `t3.sh` / `t3.d` | count-only fbt probes, 10 s: gate acquisitions per IO by call path, timer calls, interrupts, work-loop passes, distinct pool tasks |
| `cpu.sh` / `cpu.d` | CPU placement of the work loop and of the other busy processes |
| `mask.sh` | runtime A/B of the log checks: debug mask 35 / 0 / 631, 2 x 20 s each, restored to 35 |
| `analyze.py`, `roles.py`, `gate.py`, `wl.py` | stack analysis: per-component, per-role, in-gate, work-loop breakdown (us of CPU per IO) |

us/IO = samples / 4997 Hz / IOs in the window. The profile window ran at **157.3k IOPS** (interval CSV;
profile-4997 costs ~2 %): 3.146M IOs in 20 s, 12 CPUs, 1,197,769 samples. Kext PCs are symbolised with
`nm` of /Library/Extensions/SAS2308HBA.kext (UUID 085DA9D6, the rollback copy `mac/install.sh` kept).
Many kernel functions are unsymbolised (no KDK). They are classified by the frames above them, and the
two idle leaves (callers in the idle thread) are counted as idle. The fbt timing scripts with
thread-local variables (`t1`, `t2`, `sched`) lost most of their state to dynamic-variable drops and
slowed the load to 24-147k IOPS. Only their counts and distributions are used below, never their sums.

## IOPS

| run | IOPS | p50 / p99 us | note |
|---|---|---|---|
| F1 4K QD2 7 drv, r1 / r2 / r3 (30 s) | 161,940 / 160,914 / 160,877 | 80 / 144 | 4.83M / 4.81M / 4.81M IOs completed (stats), 0 errors |
| same, debug mask 35 / 0 / 631 (2 x 20 s each) | 161.1k, 160.7k / 160.5k, 160.4k / 160.3k, 160.2k | 80 / 144-148 | log checks: no effect |
| F1 4K QD2, 1 drive | 36,845 | 52 / 88 | |
| F1 4K QD1, 1 drive | 18,382 | 54 / 78 | |
| F1 4K QD1, 7 drives | 111,801 | 60 / 92 | |
| final run 2026-11-21 (same build, same boot) | 157,595 | 82 / 148 | |
| 2026-11-20 (67BF18CF, 64 commands, other boot) | 170,333 | 70 / 130 | X 4K QD8 171.2k: same ceiling at QD8 |

One drive at QD2 does 36.8k; with seven, each drive gets 23.0k and p50 rises from 52 to 80 us. The
drives are not the limit: the extra ~28 us per IO is waiting on the host, and the QD8 row hits the same
ceiling.

Interrupts (t3, 10 s at 90k IOPS under probes): 434,949 MSI for 902,406 IOs = **2.07 IOs per
interrupt**. 193,685 work-loop passes = **4.66 IOs per pass**. 1.57M MSI in 20 s at ~147k IOPS (t1) =
78.6k interrupts/s.

## Breakdown per IO (085DA9D6, 157.3k IOPS, us of CPU per IO)

Each sample is attributed to the nearest frame of a known component. "kext" therefore includes the
kernel helpers it calls (IOLock, clock reads). The kext's exclusive PCs total 0.33 us/IO.

| component | submitting threads (pread) | work-loop thread (completion) | stacks without a role | total |
|---|---|---|---|---|
| our kext SAS2308HBA | 0.74 | 0.17 | | 0.90 |
| IOSCSIParallelFamily | 0.25 | 0.06 | | 0.31 |
| SAM (IOSCSIArchitectureModelFamily, BlockCommands) | 0.42 | 0.58 | | 1.00 |
| IOStorageFamily | 2.38 | 0.51 | | 2.89 |
| DART/SPTM: map (submit), unmap (completion) | 1.54 | 1.80 | 1.26 (stacks end in SPTM) | 4.59 |
| kernel memory descriptor / wiring | 0.66 | | | 0.66 |
| **spin on the work-loop gate** | **6.86** | 0.22 | | **7.09** |
| kernel other (syscall, VFS, unsymbolised) | | 0.40 | 2.61 (+0.09 interrupt ctx) | 3.11 |
| sum of attributed stacks | 12.84 | 3.74 | 3.97 | 20.55 |

- The work-loop thread (tid 3694, runEventSources) is on CPU **4.41 us/IO, 69.4 %** of a CPU. Its
  attributed stacks cover 3.74 us; the rest are stacks that end in SPTM. 99.99 % of its passes run on
  CPUs 6-7, the two "Super" cores (perflevel0), which it shares with sasbench threads.
- **In-gate time** (stack inside runEventSources or an IOCommandGate action, not spinning): 3.20 us/IO
  held by the work loop and 0.07 by submitters.
- Other: 80,884 kernel samples (5.1 us/IO) fell outside the 600 stacks kept. User mode: sasbench 0.30
  us/IO; audiomxd 2.55 and configd 0.47 us/IO-equivalent are not IO work. Idle: 47.1 us/IO, 61.8 % of
  the machine.
- Interrupt context (filter): 0.09 us/IO. `FilterInterruptRequest` only returns true.

### The gate

`IOWorkLoop::closeGate` on the controller's work loop, counted per IO with t3 (exact counts, no state):

| call path | per IO | contended? |
|---|---|---|
| `IOBlockStorageDriver::executeRequest` -> `IOBlockStorageServices::doAsyncReadWrite` runAction | 1.00 | yes (submit thread) |
| `SendSCSICommand` -> `GetSCSIParallelTask` -> `IOCommandPool::getCommand` runAction | 1.00 | yes |
| our `ProcessTask` -> `SetTimeoutForTask` -> SCSIParallelTimer (+0.07 in `SetTimeout` itself) | 1.07 | yes |
| `CompleteParallelTask` (2 sites) from our `DeliverCompletions` | 2.00 | recursive (work loop holds it) |
| `SCSIParallelTimer::RemoveTask` in `CompleteParallelTask` | 0.82 | recursive |
| work-loop thread wake (`threadMain`) | 0.22 | |
| **total** | **6.11** | 3.07 contended per IO |

`SCSIParallelTimer::Rearm` runs 0.89 times per IO. That fits sequential IO completing oldest-first:
nearly every `RemoveTask` then takes the head of the sorted list and reprograms the timer. Lockstat, 20 s at 90k IOPS:
the top lock (the gate) had 6.21M spins (2.21 s) and 331k blocks (0.23 s). Every other lock, including
our glue lock and mps_mtx, is below 50 ms.

The submit-side spin is CPU burnt by 14 threads waiting for a work loop that holds the gate for whole
completion passes (4.66 IOs x ~4.4 us = ~20 us per pass, ~70 % of the time). Each IO waits up to three
times on the submit side and once more on the completion side, behind other IOs' completions. The
QD8 ceiling (171k before) is therefore the gate's occupancy per IO: work-loop hold plus the
submitters' handoffs. It is not the sum of CPU per IO (20 us x 157k = 3.2 CPUs of 12).

### Work loop per IO (in-gate, holder on CPU: 3.23 us)

| step | us/IO |
|---|---|
| DART unmap (`IODMACommand::complete` -> `IOGeneralMemoryDescriptor::dmaCommandOperation` -> `IODARTMapper::iovmUnmapMemory` -> `_iovmFree` -> `sptm_gen3dart_unmap`) | **1.80** (inclusive) |
| SAM completion (`TargetTaskCompletion`, `HLUNTaskCompletion`, ...) | 0.58 (+0.54 IOStorage inside it) |
| IOStorageFamily completion (`prepareRequestCompletion`, `dkreadwritecompletion`, wakeup of the pread thread) | 0.51 |
| our kext (`mps_intr_locked`, `mpssas_scsiio_complete`, `DeliverCompletions`, glue) | 0.20 |
| IOSCSIParallelFamily (`CompleteParallelTask`, `RemoveTask`, `Rearm`) | 0.06 |

### Our kext, exclusive PC samples (0.330 us/IO in all)

MapTask 0.041, callout_reset_us 0.039, auth stubs (calls out) 0.028, ProcessTask 0.027, TaskMapNext
0.024, mps_kext_submit 0.017, xpt_setup_ccb 0.014, PortTaskDescribe 0.012, Post 0.011, mps_intr_locked
0.011, mps_os_unlock 0.010, mpssas_action 0.009, mpssas_scsiio_complete 0.009, mps_enqueue_request
0.008, mtx_unlock 0.008, mps_push_sge 0.008, xpt_action 0.006, then <0.005 each: mps_os_uptime_us,
mps_xnu_io_start, **mps_xnu_target_gen (D8) 0.003**, Leave, **mpssas_log_command 0.002**,
DeliverCompletions 0.002. Nothing in the kext reaches 0.05 us/IO.

## Attribution of the delta

Candidates, checked against this build:

| candidate | evidence | cost per IO |
|---|---|---|
| D45 family lookup instead of slot table | `FindTaskFor*` is never called on the IO path (t3: 0). The old code scanned the slot table under the glue lock at submit and completion. | 0 (old cost removed) |
| D4 timer armed at post | `SetTimeoutForTask(task, 0)` is in the same place as before (review C1, before the post). D4 changed only `HandleTimeout`. | 0 new; the call itself costs a contended gate round trip (proposal 2) |
| D8 generation check at submit | `mps_xnu_target_gen` | 0.003 |
| D3 UA bookkeeping | not on the success path (`mpssas_scsiio_complete` 0.009 in all) | ~0 |
| batch E log class/level checks | mask 0 / 35 / 631: 160.4-160.5k / 160.7-161.1k / 160.2-160.3k IOPS | < noise (0.5 %) |
| batch F halt check | `MPS_XNU_HALT_CAP` inline in `mpssas_action_scsiio` (0.009 in all) | ~0 |
| D47 map-wait list | `ProcessTask` +1 glue Lock/Unlock (is anyone waiting?), `RetryMapWait` 1 Lock/Unlock per pass | ~0.02-0.04 CPU, ~0.01 in the gate |
| D55 KASSERT counting | `callout_reset_us` / `callout_stop` +6 calls each | ~0.01 |
| queue sizing (2040 tasks, 2048 commands) | family pool LIFO (14 distinct tasks). The core's command list is FIFO (rotates through 2048 commands), but no cache-miss cost shows: every core function ≤0.011 us/IO. | ~0 here (callout insert is O(outstanding), proposal 3) |
| D30, D75 | no doorbell read on empty interrupts; BAR1 posted writes on (`sas2308ctl show`) | savings, not costs |

Static diff of the hot path, EF9A8035 -> 085DA9D6 (`nm` + `objdump` of both binaries). ProcessTask
went from 1272 B / 20 calls to 852 + MapTask 580 + Post 392 B and 22 + 7 + 7 calls (the same work,
split up, plus the map-wait check). mps_kext_submit went from 636 to 816 B and 11 to 13 calls (D8, D15).
xpt_done 12 -> 15 calls, callout_reset_us/stop 14 -> 20 / 13 -> 19, bus_dmamap_load_ccb 1 -> 4 (D41,
D42). mps_intr_locked, mps_xnu_io_done, mps_complete_command, HandleInterruptRequest and
PortCompleteTask are unchanged in size. Estimate: +3-5 uncontended lock pairs and ~20 short calls
per IO, **≤0.1-0.15 us of CPU and ≤0.05 us in the gate**. Our whole completion share in the gate is
0.20 us.

**Conclusion.** The kext code added by batches A-F cannot make up 0.3-0.5 us/IO at the serial point.
The whole kext costs 0.33 us/IO, and 0.20 us of it sits inside the gate. The rest of the gap to
170.3k is not attributed. In order of likelihood:

1. The baseline: 170.3k was a different build (67BF18CF, 64 commands, before queue sizing and log
   noise) on a different boot, one run per point. Within this boot, the same build gave 157.6k and
   then 160.2-161.9k.
2. The environment: since this boot, audiomxd uses 72 % and configd 42 % CPU continuously, even with
   the drives idle (`top`), with 37k and 17k wakeups/s (sched.d). Both run on the E cores 0-5, where 35 %
   of the submitting threads' CPU time runs too (cpu.d). A submitter preempted while holding or handing over the gate
   stalls everyone (the 331k gate blocks). Nobody knows whether this was present on 11-20.
3. Cost outside the kext (DART/SPTM, IOStorage, SAM), changed by something other than our code.

Only the A/B on one boot pair settles it: EF9A8035 then 085DA9D6, the same scripts, comparing
`roles.txt`, `workloop.txt` and the t3 counts.

## Fix proposals, ranked by expected gain at this point

1. **Unmap outside the gate (expected +15-30 %, measure).** `DeliverCompletions` runs in the work
   loop with the gate held and calls `TaskUnmap` -> `IODMACommand::complete()` for each IO: 1.80 of
   the 4.41 us/IO. Doing the reply-queue pass and the `complete()` calls in a context without the gate
   (our done thread_call or a dedicated kernel thread woken by the filter), then handing the finished
   batch to the work loop only for `CompleteParallelTask`, cuts the gate's per-IO hold from ~3.2 to
   ~1.4 us. The submit-side spin, 6.9 us/IO of CPU on 14 threads, shrinks with it. Ordering stays
   correct: the unmap completes before the family hears of the IO. Check that the family accepts
   `CompleteParallelTask` from `runAction` on the controller's gate (it already runs in that gate).
2. **No family timer per IO (expected +5-10 %).** `SetTimeoutForTask` is one of the three contended
   submit-side gate acquisitions (1.07/IO). On completion it costs `RemoveTask` + `Rearm` (0.89 timer
   reprogrammings per IO) inside the gate. The core already arms a callout for every IO (timeout +
   10 s backstop, D15). Making that callout the IO timer at the task's own timeout, as FreeBSD's own
   design does, removes both. This is a trade-off against review C1 (`HandleTimeout` semantics),
   so it needs a design note.
3. **Callout insert from the tail (small here, large for deep queues).** `callout_reset_us` walks
   `mtx_callouts` from the head to keep it sorted. IO callouts carry the latest deadline, so every
   insert walks every outstanding command, under mps_mtx. That is 0.039 us/IO at 14 outstanding, but
   O(n): at 224 (QD32 x 7) to 2040 outstanding it is roughly 0.6-6 us per IO under the core lock.
   `TAILQ_FOREACH_REVERSE` makes the common case O(1). This matters for SSDs and the QD32+ rows.
4. **Glue lock trims (≤0.05-0.1 us/IO).** In `ProcessTask`, replace the map-wait Lock/Unlock with an
   atomic load of `fMapWaitHead`. In `RetryMapWait`, return before locking when the head is null. In
   `DeliverCompletions`, decrement `fOutstanding` once per pass instead of once per task under the lock.

Not ours, for the record: the other two contended submit-side gate trips (IOBlockStorageServices
power check, IOCommandPool) and the SAM/IOStorage completion cost (1.1 us/IO in the gate) are family
code. Proposal 1 makes their contention cheaper, because the gate is held less.

## How to re-measure

1. On the Mac, from the repo root: `make -C tools sasbench sas2308ctl`, then
   `sudo env TAG=<UUID8> bash bench/profile/scripts/repro.sh`. The 7 drives are
   re-derived by `sasdisks.sh`; the script refuses anything but exactly 7 unmounted Exos behind
   SAS2308HBA.
2. Find the kext base for this boot: `sudo env TAG=<UUID8> bash bench/profile/scripts/base.sh`. In `base.txt`, take the
   `CompleteParallelTask` caller and the `SetTimeout` caller and subtract the return sites (085DA9D6:
   0x141dc and 0x14870, both give the same base). For another build, find the `blraa` after
   `DeliverCompletions`/`ProcessTask` with `objdump -d` (see `gate.py`/`stk.py` helpers).
3. `sudo env TAG=<UUID8> bash bench/profile/scripts/profile.sh <base+0xc000> <base+0x32400>`, then `t3.sh`, then
   `cpu.sh <workloop>` (the top pointer under "closeGate by workloop" in `dt-t3.txt`).
4. On any machine with Python 3: `roles.py dt-prof.txt syms.txt BASE IOS`, plus `gate.py` and `wl.py`. IOS = window
   IOPS from `prof-int.csv` x 20.
5. Compare roles/workloop/t3 between builds. A fix aimed at proposal 1 should show the work-loop
   DART line move out of `runEventSources`, and the submit-side gate spin should drop.

Raw outputs (085DA9D6): `bench/profile/085DA9D6/`: run.log, results.csv, prof-int.csv, dt-prof.txt.gz
(the 600 heaviest stacks, sections `=== kst`, `=== ours`), dt-t3.txt, dt-cpu.txt, dt-t1/t2/sched/locks
(drop-affected, counts only), roles.txt, workloop.txt, gate.txt, analyze.txt, stats snapshots.
