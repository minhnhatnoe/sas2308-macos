# SAS2308HBA benchmarks and drive-cache check (2026-11-20)

Hardware: Mac mini (Mac18,5, macOS 27.0 26A425), LSI SAS 9207-8i (SAS2308, FW 20.00.07.00) in the
Thunderbolt PCIe dock (PCIe 3.0 x4 tunnel), 7 x Seagate Exos X18 ST18000NM000J (SATA 6 Gb/s, FW SN02,
512e: 512 B logical / 4 KiB physical, 7200 rpm) on PHY0, PHY2-7. Kext `org.sas2308.driver.SAS2308HBA`
0.1.0, UUID 67BF18CF, log level 2 (mask 631), loaded throughout; no driver change, no reboot.
Run 03:29-05:20 EST (Mac local time).

| disk (this boot) | serial | PHY | target |
|---|---|---|---|
| disk4 | drive1 | 0 | 0 |
| disk9 | drive2 | 2 | 1 |
| disk10 | drive3 | 3 | 2 |
| disk5 | drive4 | 4 | 3 |
| disk7 | drive5 | 5 | 4 |
| disk6 | drive6 | 6 | 5 |
| disk8 | drive7 | 7 | 6 |

## Summary
- **Per drive** (alone, 1 MiB sequential): 273-280 MB/s outer, 131-136 MB/s inner, reads = writes;
  `dd` agrees (277.9 / 125.6 MB/s).
- **Aggregate**: 7 drives 1945 MB/s read, 1940 MB/s write outer (exactly 7 x one drive), 933 /
  931 MB/s inner. Mixed 4W/3R and 3W/4R: 1942 MB/s, no drive slowed by the others. **Path
  ceiling** measured with drive-cache hits: **3.17 GB/s** (7 x 453 MB/s), so the drives, not the
  card or the Thunderbolt tunnel, limit every HDD pattern.
- **Random** (7 drives, 2-50 %): 4 KiB read 701 IOPS at QD1 (9.7 ms), 1220 at QD8, 1235 at QD32
  (queue-limited: 64 commands for all drives); 64 KiB read 700 / 1153 / 1172 IOPS.
- **Queue limit**: QD32 on 7 drives behaves like QD8 (~21 % IOPS lost) and single requests wait
  up to 49 s above the driver when 84+ commands are requested; driver max latency 2.75 s.
  Size the limit to >= 224. Separately, ~171k IOPS host-side ceiling for 4 KiB.
- **Integrity (G)**: **0 mismatches** in 6 runs (early, re-check after 85 min, final; 7 drives x 3
  zones, 84 and 168 commands requested), ~95 GiB written, ~142 GiB verified.
- **Caches**: write cache and look-ahead enabled on all 7; write-back effective (4 KiB QD1
  16,907 IOPS vs 118 with a flush after each write); SYNCHRONIZE CACHE reaches the driver 1:1
  and takes one revolution (7.7 ms median); read-cache hits 0.06 ms vs 9.5 ms seeks. FUA not
  reachable from macOS for these disks.
- **Soak** 60 min, 70 commands: 400 MB/s sequential + 900 IOPS random, flat, 0 errors.
- **Health**: IOC 59-61 C, drives 28-35 C; 17.4 M IOs with 0 errors, timeouts, recoveries,
  resets or faults; SMART error counters unchanged; no error lines in the driver log.

## Method
- `tools/sasbench.c` (built on the Mac: `clang -O2 -pthread`). Raw `/dev/rdiskN`, `F_NOCACHE`,
  16 KiB-aligned buffers, `pread`/`pwrite`. One *job* = device + mode + zone, driven by `-q N`
  threads (= queue depth of that job). Sequential jobs share one cursor, so N threads keep N
  consecutive blocks in flight. Zones: outer = 0-2 %, middle = 49-51 %, inner = 98-100 % of
  the capacity, or any `A-B` percent range. Reports per job and aggregate MB/s, IOPS and the
  latency p50/p99/max of each `pread`/`pwrite` as seen by the process (includes any queueing
  above the driver). Results go to `bench/results.csv`.
- Verify mode (`v`): every 4 KiB block is stamped with magic, its 4 KiB block number, a seed and
  the drive serial, plus a payload derived from all three. The job writes the region, then reads
  it back and compares. Check mode (`c`) only reads back a region written earlier. A mismatch is
  classified: other drive's data, wrong block (misdirected or stale request), old seed (write
  lost / stale data), payload corruption. Each run uses a new seed (`date +%s`), so data from an
  earlier run cannot pass as correct.
- Before each phase, `bench/scripts/setup.sh` re-derived the drive list (IORegistry descendant
  of SAS2308HBA, `diskutil info` Protocol SAS, model ST18000NM000J, 18000207937536 bytes,
  External), refused unless exactly 7 qualified and ran `diskutil unmountDisk` on them. The
  device list was the same all night.
- `bench/scripts/monitor.sh` (as root) sampled `sas2308ctl stats` every 15 s
  (`bench/monitor.csv`) and SMART attribute 194 of every drive every 60 s
  (`bench/drivetemps.csv`), and would have killed the benchmark on IOC temperature >= 90 C, a
  state other than operational, or any increase of IO timeouts, family timeouts, recoveries,
  reinits, IOC faults, diag resets, transport/terminated/SCSI-status/device-gone errors, stale
  or bad replies, OS TM requests, chain alloc failures. It never fired.
- Statistics were reset at 03:29 (`sas2308ctl reset-stats`; the values before are in
  `bench/stats-before-reset.txt`).
- Cross-check with `dd if=/dev/rdisk9 of=/dev/null bs=1m count=4096`: 277.9 MB/s (sasbench:
  277.3 MB/s); last 336 MiB of the disk: 125.6 MB/s.

## A. Per-drive baseline (one drive at a time, 1 MiB sequential, QD2, 20 s)
MB/s:

| drive (serial) | read outer | write outer | read inner | write inner | p99 ms (outer/inner read) | max ms (all four) |
|---|---|---|---|---|---|---|
| disk4 (drive1) | 277.0 | 278.0 | 130.9 | 132.3 | 9.0 / 17.4 | 100 |
| disk9 (drive2) | 277.3 | 275.7 | 132.7 | 133.4 | 8.7 / 17.4 | 125 |
| disk10 (drive3) | 278.7 | 280.0 | 134.9 | 135.7 | 9.0 / 16.9 | 103 |
| disk5 (drive4) | 280.3 | 280.2 | 133.4 | 134.4 | 8.7 / 17.4 | 85 |
| disk7 (drive5) | 273.6 | 273.2 | 131.4 | 132.1 | 9.0 / 16.9 | 113 |
| disk6 (drive6) | 276.3 | 277.8 | 131.8 | 132.7 | 9.0 / 17.4 | 111 |
| disk8 (drive7) | 279.6 | 280.3 | 132.5 | 133.7 | 8.7 / 17.4 | 91 |

All seven drives are within 3 % of each other: ~277 MB/s outer, ~133 MB/s inner, reads and
writes alike, p99 9 ms / 17 ms (one 1 MiB transfer per 7.6 / 15.6 ms).

## B. Scaling (1 MiB sequential, QD2 per drive, outer, 30 s)

| run | jobs | read MB/s | write MB/s | IOPS | p50 ms | p99 ms | max ms | per-job MB/s |
|---|---|---|---|---|---|---|---|---|
| B-read-1drv | 1 | 277 | 0 | 264 | 7.6 | 8.7 | 29 | 277-277 |
| B-read-2drv | 2 | 557 | 0 | 531 | 7.4 | 9.0 | 25 | 277-280 |
| B-read-4drv | 4 | 1109 | 0 | 1058 | 7.4 | 9.0 | 44 | 273-280 |
| B-read-7drv | 7 | 1945 | 0 | 1855 | 7.4 | 9.0 | 33 | 274-280 |
| B-write-1drv | 1 | 0 | 277 | 265 | 7.6 | 8.7 | 33 | 277-277 |
| B-write-2drv | 2 | 0 | 556 | 530 | 7.4 | 9.0 | 87 | 277-279 |
| B-write-4drv | 4 | 0 | 1112 | 1061 | 7.4 | 9.0 | 50 | 274-281 |
| B-write-7drv | 7 | 0 | 1940 | 1850 | 7.4 | 9.0 | 112 | 273-280 |

Linear: 7 drives = 1945 MB/s read, 1940 MB/s write = 7.0 x one drive, every drive 273-280 MB/s. The drives, not the card or the Thunderbolt tunnel, are the limit (the path ceiling, ~3.17 GB/s, is in X below). Disk numbers per drive count: 1 = disk9, 2 = +disk10, 4 = +disk5, disk7.

## C. Write-all then read-all (7 drives at once, 1 MiB, QD2, 30 s)

| run | jobs | read MB/s | write MB/s | IOPS | p50 ms | p99 ms | max ms | per-job MB/s |
|---|---|---|---|---|---|---|---|---|
| C-write-all-outer | 7 | 0 | 1946 | 1855 | 7.4 | 9.0 | 62 | 273-280 |
| C-read-all-outer | 7 | 1942 | 0 | 1852 | 7.4 | 9.0 | 37 | 273-280 |
| C-write-all-inner | 7 | 0 | 931 | 888 | 16 | 17 | 137 | 132-135 |
| C-read-all-inner | 7 | 933 | 0 | 890 | 16 | 17 | 44 | 132-136 |

Same as 7 x the single-drive numbers, outer and inner; p99 equal to one drive alone.

## D. Mixed load

| run | jobs | read MB/s | write MB/s | IOPS | p50 ms | p99 ms | max ms | per-job MB/s |
|---|---|---|---|---|---|---|---|---|
| D-4W3R-outer | 7 | 831 | 1112 | 1853 | 7.4 | 9.0 | 79 | 274-280 |
| D-3W4R-outer | 7 | 1110 | 832 | 1851 | 7.4 | 9.0 | 100 | 273-280 |
| D-mix70-seq-1M-q4 | 7 | 967 | 413 | 1315 | 16 | 56 | 124 | 195-200 |
| D-mix70-rand-64K-q8 | 7 | 68 | 29 | 1485 | 25 | 156 | 310 | 13-14 |

4W/3R and 3W/4R: every drive at its single-drive rate whatever the others do (writers and readers do not disturb each other through the card). Per-drive 70/30 sequential mix (QD4, both directions on the same cursor region 0-1.5 %) costs ~30 % (direction changes); 64 KiB random 70/30 at QD8 is seek-bound (~210 IOPS/drive).

## E. Zone mixes

| run | jobs | read MB/s | write MB/s | IOPS | p50 ms | p99 ms | max ms | per-job MB/s |
|---|---|---|---|---|---|---|---|---|
| E-write-4outer-3inner | 7 | 0 | 1476 | 1407 | 7.6 | 17 | 96 | 132-274 |
| E-read-4outer-3inner | 7 | 1508 | 0 | 1438 | 7.6 | 17 | 131 | 131-280 |
| E-read-outer+inner-same-drive | 14 | 1416 | 0 | 1350 | 7.7 | 17 | 2573 | 57-155 |
| E-write-outer+inner-same-drive | 14 | 0 | 1436 | 1370 | 7.7 | 100 | 2634 | 53-163 |
| E-readouter+writeinner-same-drive | 14 | 601 | 636 | 1180 | 13 | 25 | 2662 | 25-121 |

Drives working in different zones run at their own zone rate (4 outer ~274 + 3 inner ~132 = 1476/1508 MB/s). Two streams on one drive (outer + inner) still reach ~200 MB/s per drive because the drive's NCQ batches each stream, but the far stream can wait up to 2.6 s (max column; the same 2.6 s as the driver's own latency max, so it is inside the drive).

## F1. Block-size sweep (sequential read, 7 drives, QD2 per drive, outer, 20 s)

| run | jobs | read MB/s | write MB/s | IOPS | p50 ms | p99 ms | max ms | per-job MB/s |
|---|---|---|---|---|---|---|---|---|
| F-seqread-4K-q2 | 7 | 698 | 0 | 170333 | 0.1 | 0.1 | 155 | 99-100 |
| F-seqread-64K-q2 | 7 | 1943 | 0 | 29643 | 0.4 | 0.8 | 42 | 273-280 |
| F-seqread-128K-q2 | 7 | 1943 | 0 | 14827 | 0.9 | 1.3 | 44 | 273-280 |
| F-seqread-1M-q2 | 7 | 1935 | 0 | 1846 | 7.4 | 9.0 | 51 | 272-280 |
| F-seqread-4M-q2 | 7 | 1912 | 0 | 456 | 15 | 43 | 5070 | 268-277 |

From 64 KiB up the drives are media-bound (1.94 GB/s). 4 KiB reaches 170,000 IOPS (24,300 per drive, 100 MB/s each, all from the drives' read-ahead) — the IOPS ceiling discussed under the queue limit. 4 MiB requests are split into 1 MiB transfers (IOMaximumByteCountRead = 1 MiB), so they behave like 1 MiB at QD8; one 5.1 s max.

## F2. Random IO (7 drives unless `-1drv`; zone 2-50 % unless `fullstroke`; 20 s each)

| run | jobs | read MB/s | write MB/s | IOPS | p50 ms | p99 ms | max ms | per-job MB/s |
|---|---|---|---|---|---|---|---|---|
| F-randread-4K-qd1 | 7 | 3 | 0 | 701 | 9.7 | 18 | 89 | 0-0 |
| F-randwrite-4K-qd1 | 7 | 0 | 50 | 12201 | 0.3 | 2.0 | 27 | 7-7 |
| F-randread-4K-qd8 | 7 | 5 | 0 | 1220 | 32 | 172 | 387 | 1-1 |
| F-randwrite-4K-qd8 | 7 | 0 | 19 | 4686 | 12 | 20 | 89 | 3-3 |
| F-randread-4K-qd32 | 7 | 5 | 0 | 1235 | 160 | 319 | 20121 | 1-1 |
| F-randwrite-4K-qd32 | 7 | 0 | 19 | 4705 | 46 | 57 | 20013 | 3-3 |
| F-randread-64K-qd1 | 7 | 46 | 0 | 700 | 9.7 | 16 | 67 | 6-7 |
| F-randwrite-64K-qd1 | 7 | 0 | 26 | 399 | 24 | 42 | 135 | 4-4 |
| F-randread-64K-qd8 | 7 | 76 | 0 | 1153 | 36 | 168 | 344 | 10-11 |
| F-randwrite-64K-qd8 | 7 | 0 | 15 | 232 | 238 | 385 | 874 | 2-2 |
| F-randread-64K-qd32 | 7 | 77 | 0 | 1172 | 172 | 336 | 20117 | 11-11 |
| F-randwrite-64K-qd32 | 7 | 0 | 155 | 2369 | 72 | 819 | 15061 | 22-23 |
| F-randread-4K-qd1-1drv | 1 | 0 | 0 | 102 | 9.7 | 16 | 51 | 0-0 |
| F-randread-4K-qd8-1drv | 1 | 1 | 0 | 169 | 34 | 184 | 332 | 1-1 |
| F-randread-4K-qd32-1drv | 1 | 1 | 0 | 223 | 100 | 557 | 945 | 1-1 |
| F-randread-4K-qd1-fullstroke | 7 | 2 | 0 | 568 | 12 | 22 | 119 | 0-0 |
| F-randread-4K-qd32-fullstroke | 7 | 4 | 0 | 998 | 205 | 369 | 20156 | 1-1 |

Random reads behave like any 7200 rpm disk: ~100 IOPS per drive at QD1 (9.7 ms), ~175 at QD8 with NCQ. Random-write numbers depend on how full the drive write cache is when the run starts (runs were back to back): QD1 4 KiB writes were absorbed by the cache (12,201 IOPS, 0.3 ms), later runs at QD8/QD32 found it full. 64 KiB random writes at QD32 (155 MB/s) beat QD8 (15 MB/s) because the cache could coalesce/reorder more; treat these write rows as cache behaviour, not media IOPS.

## X. Path ceiling: drive read-cache hits (same ~5 MiB re-read, 15 s)

| run | jobs | read MB/s | write MB/s | IOPS | p50 ms | p99 ms | max ms | per-job MB/s |
|---|---|---|---|---|---|---|---|---|
| X-cachehit-read-1M-q4-1drv | 1 | 502 | 0 | 479 | 8.2 | 9.2 | 85 | 502-502 |
| X-cachehit-read-1M-q4-2drv | 2 | 1064 | 0 | 1014 | 7.4 | 9.0 | 31 | 505-559 |
| X-cachehit-read-1M-q4-4drv | 4 | 2188 | 0 | 2086 | 7.4 | 9.0 | 29 | 509-560 |
| X-cachehit-read-1M-q4-7drv | 7 | 3172 | 0 | 3025 | 9.2 | 9.2 | 34 | 452-454 |
| X-cachehit-read-128K-q8-7drv | 7 | 1707 | 0 | 13022 | 2.0 | 27 | 45 | 127-507 |
| X-cachehit-read-4K-q8-7drv | 7 | 701 | 0 | 171228 | 0.3 | 0.7 | 57 | 91-133 |

With every read served from the drive cache (each drive streams at the SATA link rate, 502-560 MB/s), 1 MiB reads at QD4 scale 1 -> 2 -> 4 drives linearly and flatten at **3.17 GB/s for 7 drives** (453 MB/s each). That is the read ceiling of the HBA + PCIe 3.0 x4 Thunderbolt tunnel path (above the 2.7 GB/s estimate), 1.6 x what seven HDDs can deliver from the media. 4 KiB cache hits: 171,000 IOPS, the same as F1's 4 KiB sequential reads at a different queue depth (see queue section). The 128 KiB row was uneven: disk9 and disk5 at 500-507 MB/s, disk10 191 MB/s, the other four at exactly 127.5 MB/s (972 IOPS, p50 3.5-3.8 ms at QD8); the same drives ran at 453 MB/s with 1 MiB reads a minute earlier, so this looks like drive cache-segment behaviour for 128 KiB re-reads, not the path. Not investigated further.

## Queue limit (64 commands for all drives)
The kext reports `ReportMaximumTaskCount` = 64 (`MPSK_MAX_TASKS`), so the SCSI Parallel family
hands the driver at most 64 tasks for all seven drives together; anything beyond waits above the
driver. The driver counters cannot see that wait: `Held now`/`Held total`, `Tasks rejected` stayed
0 all night. It shows up in two places:

- **Throughput at high queue depth.** 4 KiB random reads (2-50 % zone): one drive alone gives
  102 / 169 / 223 IOPS at QD1 / 8 / 32. Seven drives give 1220 IOPS at QD8 (174 per drive, 56
  requested < 64) and 1235 at QD32 (176 per drive, 224 requested) — QD32 on seven drives is
  the same as QD8, i.e. each drive gets ~9 commands. Seven drives at a real QD32 would reach
  ~7 x 223 = 1560 IOPS, so ~21 % is lost. 64 KiB random reads: QD8 1153, QD32 1172 IOPS (same
  plateau). Full-stroke 4 KiB at QD32: 998 IOPS (143 per drive).
- **Tail latency far above the driver's.** Every run that asked for 84 or more commands had a
  process-level max latency of 13-52 s: G 1 MiB (84 requested) 18.5 / 13.1 s, G 64 KiB (168)
  39.6 / 42.3 s, G check 64 KiB (168 reads) 51.5 s, F QD32 (224) 20.0-20.2 s, 64 KiB random
  writes QD32 15.1 s. The driver's own max (post to completion, `Latency max us`) was 2.75 s for
  the whole night, and no IO or family timeout fired, so a request waited up to ~49 s for a task
  slot above the driver: that wait is not FIFO. The soak (70 requested) stayed at 2.58 s, so
  the starvation needs a clear overload of the 64 slots. With 64 or fewer requested the max was
  under 1 s, except the ~2.6 s NCQ starvation inside the drive (E, two streams per drive) and one
  5.1 s 4 MiB read (F1).

For sizing: seven SATA drives at NCQ depth 32 need 224 commands; the IOC's RequestCredit
(IOCFacts) is the hard upper bound. A limit of 256 (or per-target 32 + some for TMs/internal)
would remove both effects.

**A second, separate ceiling: ~171,000 IOPS.** 4 KiB sequential reads at QD2 per drive (14
outstanding, served from the drives' read-ahead) gave 170,333 IOPS, and 4 KiB cache-hit reads
at QD8 per drive (56 outstanding) gave 171,228 IOPS. A single drive at QD1 does 16,900 cache-hit
IOPS (59 us round trip) and one drive alone reached 32,400 in the QD8 run, so the drives are not
the limit at 7 x QD2. Something per IO on the host side costs ~5.8 us serialised (submit or
completion path, the single interrupt vector, or the family). Irrelevant for these HDDs (the
largest HDD random load needs < 2,000 IOPS), but worth profiling before SSDs are attached.

## Drive caches through the card
**State.** All seven drives: write cache enabled, read look-ahead enabled (`smartctl -g wcache
-g lookahead`, ATA IDENTIFY through the SATL), and macOS reports `WriteCacheState = Yes` on every
IOSCSIPeripheralDeviceType00 (the SBC driver reads mode page 8 WCE at start). Reading mode
page 8 directly is not possible from macOS userland (no SCSI pass-through for block devices;
the Homebrew smartctl on Darwin has no `-d scsi`).

**Write cache is effective and flushes are honoured** (disk9, QD1, sequential at 3 % unless
noted; flush = `ioctl(DKIOCSYNCHRONIZECACHE)` after every write, timed together with the write):

| test | no flush | flush after each write | ratio |
|---|---|---|---|
| 4 KiB sequential | 16,907 IOPS, p50 0.06 ms | 118 IOPS, p50 8.2 ms | 143x |
| 64 KiB sequential | 273 MB/s (4,170 IOPS) | 3.8 MB/s (58 IOPS), p50 8.4 ms | 72x |
| 1 MiB sequential | 273 MB/s | 85 MB/s, p50 12.0 ms | 3.2x |
| 4 KiB random (2-50 %) | 2,098 IOPS, p50 0.2 ms | 32 IOPS, p50 29 ms | 66x |
| 4 KiB sequential, all 7 drives | — | 833 IOPS = 119 per drive, p50 8.2 ms | |

Without a flush a 4 KiB write completes in 59 us (it only reaches the drive's DRAM). With a
flush each write costs one revolution (8.2 ms vs 8.33 ms per revolution at 7200 rpm), i.e. the
FLUSH CACHE EXT really waits for the media; a dropped or faked flush would cost microseconds.
Flushes on different drives do not serialise (7 x 119 IOPS).

`fcntl(F_FULLFSYNC)` on a raw device returns ENOTTY ("Inappropriate ioctl for device") for
every call, so the `K-*-fullfsync` rows in `bench/results.csv` are really no-flush runs (their
`errors` column counts the refused calls) and are not used. On a mounted file system
F_FULLFSYNC ends in the same DKIOCSYNCHRONIZECACHE; that path was not tested here.

**SYNCHRONIZE CACHE reaches the driver and the drive.** Trace class on for 1 s
(`sas2308ctl mask 1655`, os_log subsystem temporarily `level:debug,persist:debug`, read back
with `log show --debug`; `bench/trace-sync-excerpt.txt`): 120 `WRITE(10)` of 8 blocks and 120
`SYNCHRONIZE CACHE(10)` to target 1, strictly alternating, every one completed `scsi good`.
Driver-level latency: writes median 0.58 ms, SYNCHRONIZE CACHE median 7.7 ms (7.05-8.10 ms),
i.e. the SATL's FLUSH CACHE EXT waits for the platter. Afterwards the mask was back at 631 and
the subsystem at `INFO PERSIST_DEFAULT`. (A live `log stream --level debug` captured nothing: with
the subsystem at INFO the debug lines are not generated, and with level:debug the stream
reported only "Messages dropped"; `log show` of persisted debug lines works.)

**Read cache is effective.** QD1 4 KiB reads of the same 1 MiB: 16,895 IOPS, p50 0.06 ms, max
0.8 ms; of the same 16 KiB: 10,212 IOPS, p50 0.1 ms. Random 4 KiB reads (2-50 %): 104 IOPS,
p50 9.5 ms. Cache hits are ~160x faster than a seek. Cache-hit streaming at 1 MiB runs at the
SATA link rate (502-560 MB/s per drive, X above).

**FUA: not reachable.** The block storage drivers of these disks publish no `IOStorageFeatures`
(no Force Unit Access, Barrier or Unmap; the internal disks have them), so macOS never asks for
FUA on them and userland has no raw-device FUA call. F_BARRIERFSYNC would fall back to a full
flush. Not tested.

## G. Data integrity
**0 mismatches in every run** — 21 jobs per run (7 drives x 3 zones), all 7 drives concurrently:

| run | per drive | blocks of 4 KiB checked (all drives) | requested commands | MB/s W / R (aggregate) | max latency | mismatches |
|---|---|---|---|---|---|---|
| G-early 1 MiB, seq, QD4/job, zones 1.8-2 / 50.8-51 / 99.8-100 % | 3 x 2 GiB | 11,010,048 | 84 | 623 / 623 | 18.5 s | 0 |
| G-early 64 KiB, permuted order, QD8/job, zones 1.5-1.8 / 50.5-50.8 / 99.5-99.8 % | 3 x 256 MiB | 1,376,256 | 168 | 72 / 72 | 39.6 s | 0 |
| G-check-early 1 MiB (re-read of G-early after 85 min of other load, no writes) | 3 x 2 GiB | 11,010,048 | 84 | — / 1193 | 5.6 s | 0 |
| G-check-early 64 KiB (same) | 3 x 256 MiB | 1,376,256 | 168 | — / 105 | 51.5 s | 0 |
| G-final 1 MiB (new seed, same zones) | 3 x 2 GiB | 11,010,048 | 84 | 623 / 623 | 13.1 s | 0 |
| G-final 64 KiB (new seed) | 3 x 256 MiB | 1,376,256 | 168 | 69 / 69 | 42.3 s | 0 |

~95 GiB written and ~142 GiB read back and compared in total; seeds early 1795164094/5, final
1795169405/6 (`bench/seeds`). Each stamp carries the serial, the block number and the seed, so
a stale request frame (wrong LBA, wrong direction, other target's data, lost write) would have
shown up as wrong-block / other-drive / old-seed mismatches. The runs deliberately overload
the 64 task slots (84 and 168 requested) so the SMIDs are reused as fast as possible across
targets. The G-check runs also show that none of the ~1.6 TB written by the other phases
landed in the G regions. This does not prove the missing arm64 barrier before the doorbell write
is harmless (it is a timing window, not a deterministic fault), but no corruption was observed
in 17.4 million IOs over the night.

## H. Soak
60 min (04:08-05:08), two processes at once on all seven drives: (1) 1 MiB sequential QD2 —
disk4/9/10/5 writing 0-1.5 %, disk7/6/8 reading 98-99.5 %; (2) 64 KiB random 70/30 read/write
QD8 in 2-50 % on all seven. 10 threads per drive, 70 requested commands.

| stream | avg MB/s (30 s intervals) | min | max | max latency | errors |
|---|---|---|---|---|---|
| sequential 1 MiB (4 W + 3 R) | 400.5 | 307 | 508 | 2.57 s | 0 |
| random 64 KiB 70/30 | 59.0 (900 IOPS) | 50 | 68 | 2.58 s | 0 |

Per job: sequential writers 66-80 MB/s, sequential readers 15-53 MB/s (the random stream on the
same drive takes most of the head time; the reads are starved more than the writes, which the
write cache absorbs), random 5.2-7.7 MB/s read + 2.2-3.3 MB/s write per drive. Throughput was
flat over the hour (`bench/soak-*.csv`). No error, timeout, recovery, held IO, reinit or fault;
IOC 59-61 C, drives up to 35 C. The first 28 s ran with the trace class on (see anomalies).

## Temperatures, counters, logs
- **IOC temperature** (`bench/monitor.csv`, 427 samples, 03:29-05:20): 59-61 C the whole night
  (59 C x39, 60 C x79, 61 C x308), the same as idle. No load dependence at this scale.
- **Drive temperatures** (SMART 194, 749 readings): 28-29 C at the start, 32-35 C under load;
  max per drive disk10/disk7/disk9 35 C, the others 34 C.
- **Driver counters at the end** (`bench/stats-end.txt`, since the 03:29 reset): 17,360,996 IOs,
  1.47 TB read, 1.72 TB written; IO timeouts, family timeouts, TM timeouts, recoveries, reinits,
  IOC faults, check conditions, SCSI-status/terminated/transport/device-gone errors, stale and
  bad replies, held IO, rejected tasks: all 0. Latency max 2.75 s (inside the drive, NCQ),
  chain free low-water 2087 (never short), 121 "recovered sense (not errors)". State operational.
- **SMART error attributes** (1, 5, 187, 188, 197, 199) identical at 04:11 and 05:20 on all seven
  (`bench/smart-mid.txt`, `smart-end.txt`): no new CRC, timeout or reallocation counts.
- **Driver log** (`log show` from 03:29): no error/fault/recovery line. 221 event pairs "Working
  on Event: [ff02]" / "Event Free" (one every ~30 s, all night), 1 statistics-reset line, 11
  "debug mask ... set from userland" lines (logged with type ERROR, see anomalies).
- The watchdog never fired; `ALERTS.txt` stayed empty.

## macOS without code (feature-ideas section 0)
- **SMART status: yes.** `diskutil info` shows `SMART Status: Verified` for all seven, and
  System Information > SAS shows `S.M.A.R.T. status: Verified` per logical unit. The Homebrew
  `smartctl` 7.5 reads them as ATA without any `-d` option and without root: identity, all
  attributes, SCT status (temperatures), `-H` PASSED for all seven. It goes through the driver (the
  `IOs completed` counter moves with each call). `smartctl -l scterc` fails with "SMART WRITE LOG
  does not return COUNT and LBA_LOW register": this pass-through path does not return the ATA
  output registers, so SCT commands that need them (ERC get/set) do not work.
- **Write cache:** enabled on all seven (see the cache section); macOS `WriteCacheState = Yes`.
- **`system_profiler SPSASDataType`** (`bench/sp_sas.txt`): SAS Domain 0 = LSI SAS9207-8i,
  revision 20.00.07.00, initiator identifier 64, speed 6 Gigabit, then per target the SAS
  address, target ID, peripheral type, ATA model/revision, and per LUN capacity, serial,
  "SATA Device: Yes", BSD name, partition map, S.M.A.R.T. status and volumes. It shows the
  Controller Characteristics `Port Description` string verbatim: "1-port SAS2 HBA, MPI 2.0,
  Thunderbolt" — design.md says "8-port"; the published string should probably say 8-port.
- Transfer size: `IOMaximumByteCountRead/Write` = 1 MiB per disk, 257 segments; larger requests
  are split by the block layer (4 MiB reads in F1).
- `pmset disksleep` is 10 min. The drives were never idle that long during the run (warm-up
  reads before each phase had max 19-24 ms, no spin-up seen), so spin-down was not exercised.

## Anomalies and notes
1. **Queue limit starvation** (above): with 84-224 commands requested, single requests waited up
   to 49 s above the driver while the driver's max was 2.75 s. Not a driver fault, but it is what
   applications would see with several busy drives; raising `MPSK_MAX_TASKS` fixes it.
2. **~171k IOPS host-side ceiling** (above), independent of queue depth.
3. **NCQ starvation inside the drive, ~2.6 s**: whenever one drive has two competing streams
   (E, G, soak) a few requests take 2.5-2.75 s at the driver. Drive behaviour, consistent across
   drives; far below the 30 s timeout.
4. **`F_FULLFSYNC` on a raw device returns ENOTTY**; use `DKIOCSYNCHRONIZECACHE` there.
5. **Script bug, trace class left on for 2 min 15 s**: `phaseCache.sh` read the old mask with the
   wrong key ("Debug mask" instead of `SAS2308DebugMask`), so the restore was skipped and the
   mask stayed 0x677 (trace) from 04:06:34 to 04:08:49, when it was set back to 631 by hand. This
   covers `K-trace-*`, all `K2-*` runs and the first 28 s of the soak; the trace-on 4 KiB QD1
   write rate was ~9 % lower (15,457 vs 16,907 IOPS). The K2 numbers are flush-bound and still
   valid. The trace lines themselves were not captured (subsystem at INFO). Fixed for K3/K4.
6. **Driver logs "debug mask ... set from userland" with os_log type ERROR**: it is a normal
   user action; DEFAULT/info would be more fitting.
7. **Port Description says "1-port SAS2 HBA"** in Controller Characteristics / System
   Information, while the card has 8 PHYs (design.md says "8-port").
8. **Event 0xff02 every ~30 s** (221 times): logged at the event class, harmless, but the event
   code is not decoded; worth identifying (and maybe quieting) later.
9. **macOS reads LBA 0 after a raw writer closes the device**: 5 `READ(10) lba 0 blocks 1` in the
   trace right after the flush test ended (DiskArbitration re-probe). Harmless; no "disk not
   readable" handling was needed. disk4's GPT/exFAT partition was overwritten by the outer-zone
   writes (allowed by the test plan); disk4 will now show as uninitialised.
10. `smartctl -l scterc` fails (pass-through does not return ATA output registers).
11. The X 128 KiB cache-hit row was uneven between drives (drive cache behaviour, not studied).

## Queue depth with 2040 commands (branch queue-freebsd, kext A42837FD, 14:52-15:06 EST)
FreeBSD queue sizes (docs/design.md "Queue sizes as FreeBSD"). The card reports
RequestCredit 10240, HighPriorityCredit 128, MaxReplyDescriptorPostQueueDepth 65504,
MaxChainDepth 128, 128-byte request frames, MaxTargets 1024 (`bench/q2048/controller-limits.txt`);
the driver uses 2048 + 128 frames and reports 2040 tasks (`IOCommandPoolSize` 2040, was 256).
All 7 drives at once, 120 s per point, zone 2-50 % for random IO, outer zone for sequential.
"Driver latency" = the driver's own post-to-completion average over the run (from
`SAS2308Statistics` IOs completed / Latency avg before and after).

| point | QD per drive (total) | IOPS | MB/s | p50 | p99 | max | driver latency |
|---|---|---|---|---|---|---|---|
| random 4K read | 32 (224) | 1748 | 7.2 | 98 ms | 483 ms | 0.87 s | 128 ms |
| random 4K read | 64 (448) | 1742 | 7.1 | 225 ms | 606 ms | 0.91 s | 257 ms |
| random 4K read | 128 (896) | 1738 | 7.1 | 483 ms | 868 ms | 1.22 s | 514 ms |
| random 4K write | 32 (224) | 5843 | 23.9 | 46 ms | 61 ms | 0.14 s | 39 ms |
| random 4K write | 64 (448) | 5650 | 23.1 | 94 ms | 115 ms | 0.19 s | 79 ms |
| sequential 1 MiB read | 2 (14) | 1852 | 1941.5 | 7.4 ms | 9.0 ms | 83 ms | 7.9 ms |
| verify 1 MiB seq, 1 GiB/drive | 4 (28) | 1819 | 1907 (953 W + 953 R) | 15 ms | 17 ms | 2.5 s | - |
| verify 64 KiB random, 256 MiB/drive | 64 (448) | 1034 | 67.8 | 49 ms | 2.2 s | 3.2 s | - |

- Previous build (256 commands, entry 63): random 4K read QD32 1707 IOPS, worst 0.91 s;
  sequential read 1945 MB/s. No regression.
- **IOPS do not rise beyond QD32** (1748 → 1742 → 1738) while latency doubles with each
  doubling of the queue. Little's law with the driver latency gives 224, 448 and 896 commands
  outstanding below the driver, i.e. every command really is at the card: neither macOS nor
  the driver throttles. The extra commands only wait (in the card or the drive's queue), they
  do not give the drive a better seek order: ~250 random 4K reads per drive over half the
  stroke is the drive's limit, reached by QD32. Deeper queues cost latency only.
- Random writes: QD64 is not faster than QD32 (write cache full, as in F2).
- Verify: 0 mismatches (7 x 1 GiB sequential at QD4, 7 x 256 MiB random 64 KiB at QD64).
  The 2-3 s maxima are writes in verify mode (drive write cache), no timeouts.
- Counters over all points: IO timeouts 0, Family timeouts 0, TMs 0, recoveries 0, reinits 0,
  diag resets 0, IOC faults 0, held 0 (1 at boot), tasks rejected 0, chain alloc failures 0,
  SCSI status errors 0, state operational. IOC 61-62 C, drives 30-34 C.
- Two points first failed with `Resource busy` on rdisk9 / rdisk10 at open (Disk Arbitration
  had the JHFS+ test disks open right after the previous write point); rerun with an
  `unmountDisk` and a retry before each point (`bench/scripts/q_bench2.sh`).
- Files: `bench/q2048/` (results.csv, run.log.gz, monitor.csv, drivetemps.csv, stats, limits);
  scripts `bench/scripts/q_bench.sh`, `q_bench2.sh`, `q_monitor.sh` (monitor.sh without
  "Err SCSI status" / chain failures in the kill list).

## Files
- `tools/sasbench.c` — the benchmark (usage in its header).
- `bench/scripts/` — `sasdisks.sh` (drive qualification), `setup.sh`, `monitor.sh`, `lib.sh` and
  the phase scripts exactly as run (`phaseA`, `phaseG`, `phaseGcheck`, `phaseBCDEF`,
  `phaseCache`, `phaseK2`, `phaseK3`, `phaseK4`, `phaseH`). The quoted SYNCHRONIZE CACHE trace
  was taken by hand like `phaseK4`, but with `log config --mode level:debug,persist:debug` and
  `log show --debug`.
- `bench/results.csv` — one line per job and one aggregate (`ALL`) line per run.
- `bench/soak-seq.csv`, `bench/soak-rand.csv` — soak time series (30 s).
- `bench/monitor.csv`, `bench/drivetemps.csv` — sampler output.
- `bench/run.log.gz` — the full run log; `bench/trace-sync-excerpt.txt` — trace excerpt;
  `bench/stats-*.txt`, `bench/smart-*.txt`, `bench/sp_sas.txt`, `bench/seeds`, `bench/warm.csv`.
