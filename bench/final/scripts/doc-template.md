# SAS2308HBA final benchmarks and thermal soak (2026-11-20/21)

Hardware as on 2026-11-20 (`docs/benchmarks-20261120.md`): Mac mini Mac18,5 (macOS 27.0 26A425), LSI
SAS 9207-8i (SAS2308, FW 20.00.07.00) in the Thunderbolt PCIe dock, 7 x Seagate Exos X18
ST18000NM000J (SATA 6 Gb/s, 512e, 7200 rpm). Kext SAS2308HBA 0.1.0, **UUID
085DA9D6-7832-3034-AAB2-460054CDFFA8 = master 4ce58a7** (batches A-F), 2040 tasks
(`IOCommandPoolSize`), log level 2, debug mask 35. Loaded throughout, no reboot, no debug triggers.
Run 2026-11-20 23:41 - 2026-11-21 01:00 EST: performance part 23:41-00:31 (50 min), thermal soak
00:31-01:00. **The soak load was stopped by hand after 21 min 21 s of the planned 30 min**
(00:33:14-00:54:35), followed by the full 5 min cool-down.

**Build check.** Loaded UUID 085DA9D6. Checksum `rsync -n -c` of master against the Mac build tree:
only `docs/` differ, no code. A fresh `make clean; make; make check` on the Mac gives UUID 798EBED2,
but `__TEXT_EXEC,__text`, `__TEXT,__cstring/__const/__os_log`, `__DATA,__data`,
`__DATA_CONST,__const/__got/__auth_got` and all non-debug symbols are byte-identical to the loaded
binary; the only differences are the 16 N_OSO debug-map entries (object file mtimes), hence the UUID
and the code signature. So the loaded kext is master code; no install was needed.

| disk (this boot) | serial | PHY | target |
|---|---|---|---|
| disk4 | drive1 | 0 | 0 |
| disk5 | drive2 | 2 | 1 |
| disk6 | drive3 | 3 | 2 |
| disk7 | drive4 | 4 | 3 |
| disk8 | drive5 | 5 | 4 |
| disk9 | drive6 | 6 | 5 |
| disk10 | drive7 | 7 | 6 |

Disk numbers differ from 2026-11-20 (then disk4/9/10/5/7/6/8); all per-drive tables use the serial.
The single-drive tests (B 1 drive, K, F 1drv) use drive2 as before.

## Summary
- **No regression anywhere the old and new run measure the same thing.** Per drive 272-280 MB/s outer,
  132-136 MB/s inner (all 28 A points within 1 % of before); 7 drives 1945 MB/s read / 1948 MB/s
  write outer, 934 / 931 inner; mixed and zone patterns within 2 %.
- **Deeper queue (2040 tasks instead of 64) shows where it should:** 7-drive random 4 KiB read QD32
  **1626 IOPS (+32 %)**, 64 KiB QD32 1538 (+31 %), full-stroke QD32 1392 (+40 %), and the 15-20 s
  process-level tail latencies of the old QD32 runs are gone (max 1.1-1.6 s). The queue-depth table
  matches the q2048 run (1755 / 1764 / 1761 IOPS at QD32/64/128). G integrity runs +5 % / +67 %.
- **Path ceiling, reads: 3.171 GB/s** (7 drives x 453 MB/s from drive cache), unchanged, = **95.6 %**
  of the PCIe link math for the configured MPS: 3.94 GB/s x 128/(128+24) = 3.32 GB/s. **Writes:
  2.81 GB/s** with 7 drives, but that is 7 x 401 MB/s, the drives' own cache-write rate (linear
  1/2/4/7 = 395/791/1618/2809 MB/s), so the write path was not saturated; flat for 60 s, no throttling.
- **Caches:** write-back effective (4 KiB QD1 16,559 IOPS vs 118 with a SYNCHRONIZE CACHE after each
  write = one revolution); file-system `F_FULLFSYNC` on the JHFS+ volume: 42 IOPS (25 ms) vs
  fsync() 1518 IOPS; `F_BARRIERFSYNC` = `F_FULLFSYNC` (no barrier support). Write cache cannot be
  switched off through this path (smartctl: "Unsupported ATA feature"). SYNCHRONIZE CACHE traced
  1:1 with the writes, 7.66 ms median at the driver (one revolution), all `scsi good`.
- **Integrity: 0 mismatches** (G 1 MiB + 64 KiB, G re-check after ~33 min of other load, Q verify).
- **Thermal:** nothing heats up. IOC 59 C cold idle, 61 C under every load incl. the soak (peak 62 C
  once during the performance part); drives 27-30 C cold, 31-33 C steady (reached during the
  performance part; the soak added at most 1 C); SoC die max 36 C in the soak (42.7 C peak in the performance part), thermal pressure
  Nominal throughout. Far from the 90 C / 60 C abort limits.
- **Counters** over the whole run (since reset at 23:41): 15.9 M IOs, 1.92 TB read, 1.78 TB written (incl. the post-run recheck);
  IO / family / TM / held timeouts, recoveries, reinits, diag resets, IOC faults, SCSI status /
  terminated / transport / device-gone errors, OS TM requests, lost-completion kicks, KASSERT hits,
  stale replies, chain alloc failures: **all 0**; state operational; driver latency max 3.39 s (64 KiB
  random verify at QD64, inside the drives' NCQ). No error or fault line in the driver log.

## Method
Same tool and patterns as 2026-11-20 (`tools/sasbench.c`, raw `/dev/rdiskN`, `F_NOCACHE`, zones
outer 0-2 % / inner 98-100 % / percent ranges; per-point MB/s, IOPS, process-level p50/p99/max).
Points shortened to keep the performance part at ~50 min: A 15 s (was 20), Q 60 s (was 120), H soak
pattern 3 min (was 60), others as before. Scripts as run: `bench/final/scripts/` (`run_all.sh`,
`perf.sh`, `soak.sh`, `lib_final.sh`, `monitor_final.sh`, `recheck.sh`; `fsbench.c` = file-system
fsync test, `socsens.c` = SoC temperature sensors via IOHIDEventSystem, `compare.py` = these
tables). Before every phase the drive list was re-derived (IORegistry child of SAS2308HBA +
diskutil Protocol SAS / ST18000NM000J / 18000207937536 bytes / External, exactly 7, never
disk0-3, same serial/target set as at the start) and the 7 were unmounted; every point ran
`unmountDisk` first and retried on "Resource busy" (never needed). `monitor_final.sh` sampled
`sas2308ctl stats` every 15 s and SMART 194 + SoC sensors every 60 s and would have stopped the run
on IOC >= 90 C, drive >= 60 C, a state change, or any increase of the error/timeout/recovery
counters; it never fired (`ALERTS.txt` absent). Statistics reset at 23:41
(`stats-before-reset.txt`).

Tables: "before / now" with before = 2026-11-20 (`bench/results.csv`; Q rows = q2048 run,
`bench/q2048/results.csv`); **delta on IOPS** (= on MB/s for the same block size); ** = more than
5 %. Latencies are process-level (include queueing above the driver).

## A. Per drive (one at a time, 1 MiB sequential, QD2, 15 s)
<!--T:A-->

`dd if=/dev/rdisk5 bs=1m count=4096`: 277.7 MB/s (before 277.9); last 336 MiB: 125.6 MB/s (125.6).
All within 1 %. The max column moves both ways (single spikes of 10-125 ms, drive-internal).

## B. Scaling (1 MiB sequential, QD2 per drive, outer, 30 s)
<!--T:B-->

## C. Write-all then read-all (7 drives, 1 MiB, QD2, 30 s)
<!--T:C-->

## D. Mixed load
<!--T:D-->

## E. Zone mixes
<!--T:E-->

## F1. Block-size sweep (sequential read, 7 drives, QD2 per drive, outer, 20 s)
<!--T:F1-->

4 KiB sequential reads (served from the drives' read-ahead) are 7.5 % lower: 157,600 vs 170,300
IOPS. This is the host-side per-IO ceiling of the old doc (~5.8 us per IO serialised), now ~6.3 us;
the 4 KiB cache-hit row in X shows the same -5 %. Everything from 64 KiB up is media-bound and
unchanged. See anomalies.

## F2. Random IO (7 drives unless `-1drv`; 2-50 % unless `fullstroke`; 20 s)
<!--T:F2-->

The QD32 rows are the queue-limit effect of the old 64-task build: with 2040 tasks each drive gets
its 32 commands (7 x 232 = 1626 IOPS, vs 7 x 223 = 1560 predicted from one drive), and the
process-level max drops from 20 s to 1.1-1.6 s. QD1/QD8 and the 1-drive rows are unchanged (within
5 %). Random-write rows depend on the drive write-cache fill (runs back to back) as before; treat
the +5-7 % on 64 KiB writes as cache behaviour.

## K. Drive caches (drive2, QD1; flush = `ioctl(DKIOCSYNCHRONIZECACHE)` after every write)
<!--T:K-->

Rows: `K-*` before = the valid 2026-11-20 runs (K2 for the flush rows). `rawfullfsync`:
`fcntl(F_FULLFSYNC)` on the raw device still returns ENOTTY for every call (errors column), as
before. `K-wcon-*` = after the write-cache-off attempt (below), cache still on. Flush rows: a 64 KiB
write + flush now costs one revolution (114/s, p99 17 ms) instead of two (58/s, p99 58 ms); the
7-drive 4 KiB flush row is -7 % with a 41 ms p99 (a few slow flushes); the 4 KiB, 1 MiB and random
flush rows are identical to before. Drive-side timing, flushes still 1:1 (trace below).

**Write cache on/off.** `smartctl -s wcache,off` fails on all attempts with "Write cache disable
failed: Unsupported ATA feature" (and `wcache,on` likewise); `smartctl -g wcache` keeps reporting
Enabled. The SET FEATURES pass-through does not work through this path (same family as the SCT ERC
failure in the old doc: the SATL pass-through does not return the ATA output registers smartctl
checks). So write cache off could not be measured; the flush-per-write rows stand in for it (a
4 KiB write that must reach the media costs one revolution: 8.2 ms, 118 IOPS, vs 59 us into the
cache).

**With and without F_FULLFSYNC on a file system** (new; `fsbench`, 64 MiB file on the JHFS+ volume
H5drive6 of drive6, mounted for this test only, QD1 sequential, `F_NOCACHE`, 15 s each;
`bench/final/kfs.txt`):

| test | ops/s | MB/s | p50 | p99 | max |
|---|---|---|---|---|---|
| 4 KiB, no sync | 25,000 | 102 | 0.001 ms | 0.25 ms | 37 ms |
| 4 KiB, `fsync()` | 1,518 | 6.2 | 0.36 ms | 9.5 ms | 34 ms |
| 4 KiB, `F_FULLFSYNC` | 42 | 0.17 | 25.1 ms | 33.8 ms | 50 ms |
| 4 KiB, `F_BARRIERFSYNC` | 42 | 0.17 | 25.1 ms | 33.4 ms | 70 ms |
| 1 MiB, no sync | 242 | 254 | 3.5 ms | 31 ms | 71 ms |
| 1 MiB, `F_FULLFSYNC` | 33 | 35 | 33.4 ms | 58 ms | 67 ms |

4 KiB writes are smaller than the 16 KiB page, so HFS+ takes them through the buffer cache despite
`F_NOCACHE` (the no-sync row is RAM speed, throttled at exactly 25,000/s); `fsync()` pushes the data
to the drive's cache (0.36 ms, no flush: macOS fsync does not flush the drive); `F_FULLFSYNC` adds
the journal write and a FLUSH CACHE: 25 ms = ~3 revolutions per call, 0 errors (it works on a file
system; ENOTTY only on raw devices). `F_BARRIERFSYNC` costs exactly as much: these disks publish no
barrier feature, so it falls back to a full flush.

**Read cache.** QD1 4 KiB re-reads of the same 16 KiB / 1 MiB: 4,302 / 4,530 IOPS, p50 0.10 ms,
vs 10,212 / 16,895 IOPS (0.07 / 0.05 ms) before. Still ~95x faster than a seek (random 4 KiB:
101 IOPS, 9.7 ms), so the read cache works, but each hit costs ~0.1 ms more than on 2026-11-20.
<!--RECHECK-->

**SYNCHRONIZE CACHE reaches the drive.** <!--TRACE-->

## X. Path ceiling (drive-cache hits: same ~5 MiB re-read / rewritten, 15 s; write 60 s run with 5 s intervals)
<!--T:X-->

PCIe link as reported by the driver (`ioreg` "PCIe Link"): **Gen3 (8.0 GT/s) x4** (card max x8),
**Max Payload Size 128 B** configured (device supports 4096), **Max Read Request Size 512 B**,
Device Control 0x291F.

- **Reads** (card DMA-writes to host memory as 128 B posted writes): link math 3.94 GB/s x
  128/(128+24) = **3.32 GB/s**. Measured 1 -> 2 -> 4 drives linear (541 / 537 / 560 MB/s per drive,
  at the SATA link rate), 7 drives **3.171 GB/s** (453 MB/s each, all equal) = **95.6 % of the link
  math**, the same 3.17 GB/s as before. The remaining ~4 % are DLLPs (ACK/flow-control) and the
  Thunderbolt tunnel. The bound is the 128 B payload: with MPS 256 the same math gives 3.61 GB/s.
- **Writes** (card reads host memory with 512 B read requests, completions return in <= 128 B
  TLPs): 1 / 2 / 4 / 7 drives = 395 / 791 / 1618 / **2809 MB/s**, i.e. 395-404 MB/s per drive at
  every count: the drives' cache-write rate, not the path. The 60 s run stayed at 2.81 GB/s in every
  5 s interval (2.81-2.82 GB/s after the first) - **no throttling** by the drives within 60 s of
  cached rewrites. The write ceiling of the path is therefore >= 2.81 GB/s (85 % of 3.32), not
  reached. 4 KiB cache-hit writes: 122,200 IOPS (7 drives, QD8).
- 128 KiB cache-hit reads: 1941 MB/s (+14 %), even this time (no 127.5 MB/s drives as before).

## Queue depth (all 7 drives, 60 s per point; before = q2048 run, 120 s per point)
<!--T:Q-->

Driver latency (post-to-completion, from `Latency avg us` and `IOs completed` before/after each
point, `bench/final/stats/`): random 4K read QD32 / 64 / 128 = **127 / 253 / 503 ms** (before
128 / 257 / 514), write QD32 / 64 = 32 / 98 ms (39 / 79). Same picture as q2048: IOPS stop rising at
QD32 (1755 -> 1764 -> 1761) and the latency doubles with each doubling; Little's law gives
223 / 445 / 886 commands at the card, i.e. nothing throttles above or in the driver. Random-write
QD32 +22 % / QD64 -19 % is write-cache fill (the two runs swap places depending on what ran
before), not the driver. Q verify: 0 mismatches.

## G. Data integrity (7 drives x 3 zones, all concurrently; before = G-final / G-check-early)
<!--T:G-->

**0 mismatches** in all four runs: G 1 MiB sequential QD4 (zones 1.8-2 / 50.8-51 / 99.8-100 %,
3 x 2 GiB per drive, 84 commands requested), G 64 KiB permuted QD8 (1.5-1.8 / 50.5-50.8 /
99.5-99.8 %, 3 x 256 MiB, 168 requested), and the re-read of both after ~33 min of other load
(seeds in `bench/final/seeds`). Plus Q verify 1 MiB QD4 1 GiB/drive and 64 KiB random QD64
256 MiB/drive (448 requested): 0 mismatches. ~56 GiB written and ~103 GiB read back and compared.
The 64 KiB runs are +67-74 % faster and the process max drops from 42-52 s to 2.6 s: with 168
requests and 2040 tasks nothing waits above the driver any more.

## H. The soak pattern, 3 min (was 60 min)
<!--T:H-->

Same two streams as before (1 MiB sequential QD2: 4 drives writing 0-1.5 %, 3 reading 98-99.5 %;
64 KiB random 70/30 QD8 in 2-50 % on all seven). The split moved: sequential 1223 MB/s (was 401),
random 282 IOPS (was 900). With 64 tasks for 70 requests the random stream used to get its share
of the drives' queues; with 2040 tasks each drive holds all 10 commands and its NCQ favours the
sequential stream (short seeks), so the random stream is starved (p99 2.49 s). Total work per drive
rose (sequential bytes x3), so this is a fairness shift inside the drives, not a loss. 0 errors.

## Thermal soak
Load: all 7 drives, 1 MiB sequential 50/50 read/write (`m` mode, one cursor per drive over the full
LBA range, starting at LBA 0), QD8 per drive (56 commands). Planned 30 min, **ran 21 min 21 s**
(stopped by request), then 5 min cool-down; 2 min idle baseline before. Samples every 30 s
(`bench/final/soak.csv`): IOC temperature (`sas2308ctl stats`), SMART 194 of every drive (all 7
readable at every sample), SoC sensors (`socsens`: max and mean of the 27 PMU tdie die sensors),
thermal pressure (`powermetrics --samplers thermal`; this macOS has no `smc` sampler and no
temperature in powermetrics; `pmset -g therm` reports no thermal or performance warning ever
recorded), driver throughput, latency and error counters.

<!--THERMAL-->

- **Throughput** flat: 1305-1341 MB/s aggregate in every 30 s interval (~188 MB/s per drive,
  94 read + 94 write), 1254-1365 MB/s by the driver's byte counters; driver interval latency
  43-45 ms (56 commands / 1313 IOPS), process max 246 ms. No error, timeout, TM, reinit or held IO.
- **Time to steady state:** none needed in the soak: the drives had reached their steady 31-33 C
  during the 50 min performance part (cold 27-30 C at 23:41, 31-33 C from ~00:00), and the IOC sat
  at 61 C from the first minutes of load. Within the soak the drives rose by 0-1 C (max 33 C,
  drive3); the IOC did not move.
- **Cool-down:** <!--COOL-->
- Context: the old run saw IOC 59-61 C and drives up to 35 C over 2 h; this run is the same or
  cooler. 1.3 GB/s of mixed sequential IO is the drives' limit for this pattern, and at that
  load the SAS2308 runs at its idle temperature +2 C.

## Counters, logs
- Driver counters (`bench/final/stats-final.txt`, since 23:41, after the recheck): <!--COUNTERS-->
- Driver log (`log show`, driver subsystem, 23:41-01:05): <!--LOG-->
- `monitor_final.sh`: <!--MON--> samples, never alerted; IOC 58-62 C over the whole run.

## Anomalies and notes
1. **QD1 read-cache hits ~0.05 ms slower** (4.3-4.5k vs 10-17k IOPS, K table) while 4 KiB cache-hit
   writes at QD1 are unchanged (59 us, 16,559 IOPS) and 7 x QD8 cache-hit reads are -5 %.
   <!--RECHECKNOTE-->
2. **Host-side 4 KiB IOPS ceiling -5 to -7.5 %** (F1 4 KiB 157.6k vs 170.3k, X 4 KiB 162.5k vs
   171.2k). Small, but consistent with ~0.5 us more per IO in the submit/completion path after
   batches A-F (more accounting/logging checks per IO). Irrelevant for HDDs (< 2,000 IOPS needed);
   worth a profile before SSDs.
3. **SIGINT does not stop a backgrounded sasbench.** Non-interactive bash starts `&` jobs with
   SIGINT ignored, so `monitor.sh`'s `pkill -INT -x sasbench` (also in the 2026-11-20 monitor)
   would not have stopped a benchmark. The soak was stopped with SIGTERM (sasbench rc 143, no
   summary line; the 30 s interval CSV covers it). Fix: use SIGTERM in the watchdogs or trap INT.
4. **Write cache cannot be toggled** through this path (`smartctl -s wcache,off`: "Unsupported ATA
   feature"), see K.
5. H pattern: random stream starved by the sequential stream on the same drive now that the
   drives hold all commands (fairness inside the drive's NCQ, p99 2.49 s, max 2.6 s).
6. NCQ starvation inside the drive (~2.6 s, E rows, two streams per drive) unchanged.
7. The outer-zone writes overwrote the JHFS+ test volumes on drive6 and drive7 (allowed);
   <!--VOLS-->

## Files
- `bench/final/results.csv` (one line per job + `ALL` per run), `monitor.csv`, `drivetemps.csv`,
  `soc.csv`, `soak.csv`, `soak-int.csv`, `idle-cold*.csv`, `xwrite-60s.csv`, `h-*.csv`, `kfs.txt`,
  `wcache.txt`, `trace-sync*.txt`, `stats/` (driver stats around the Q points), `stats-*.txt`,
  `show-*.txt`, `seeds`, `disks.txt`, `run.log.gz`, `soak-temps.txt` (ASCII plot).
- `bench/final/scripts/` - the scripts as run.
