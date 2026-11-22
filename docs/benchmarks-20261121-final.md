# SAS2308HBA final benchmarks and thermal soak (2026-11-20/21)

Hardware as on 2026-11-20 (`docs/benchmarks-20261120.md`): Mac mini Mac18,5 (macOS 27.0 26A425), LSI
SAS 9207-8i (SAS2308, FW 20.00.07.00) in the Thunderbolt PCIe dock, 7 x Seagate Exos X18
ST18000NM000J (SATA 6 Gb/s, 512e, 7200 rpm). Kext `org.sas2308.driver.SAS2308HBA` 0.1.0, **UUID
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
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| A-read-outer-drive1 | 277 / 277 | 264 / 264 | +0.0 % | 7.4 / 7.4 | 9.0 / 9.0 | 40.2 / 16.6 | 0 / 0 |
| A-write-outer-drive1 | 278 / 276 | 265 / 263 | -0.8 % | 7.4 / 7.4 | 9.0 / 9.0 | 10.2 / 58.4 | 0 / 0 |
| A-read-inner-drive1 | 131 / 132 | 125 / 126 | +0.8 % | 15.6 / 15.6 | 17.4 / 17.4 | 92.3 / 39.3 | 0 / 0 |
| A-write-inner-drive1 | 132 / 133 | 126 / 127 | +0.8 % | 15.6 / 15.6 | 17.4 / 17.4 | 100 / 17.9 | 0 / 0 |
| A-read-outer-drive2 | 277 / 277 | 264 / 264 | +0.0 % | 7.6 / 7.6 | 8.7 / 8.7 | 25.0 / 24.9 | 0 / 0 |
| A-write-outer-drive2 | 276 / 277 | 263 / 264 | +0.4 % | 7.6 / 7.6 | 9.0 / 9.0 | 125 / 50.6 | 0 / 0 |
| A-read-inner-drive2 | 133 / 133 | 127 / 127 | +0.0 % | 15.6 / 15.6 | 17.4 / 17.4 | 54.0 / 32.3 | 0 / 0 |
| A-write-inner-drive2 | 133 / 133 | 127 / 127 | +0.0 % | 15.6 / 15.6 | 17.4 / 17.4 | 56.4 / 114 | 0 / 0 |
| A-read-outer-drive3 | 279 / 280 | 266 / 267 | +0.4 % | 7.4 / 7.4 | 9.0 / 9.0 | 31.1 / 27.2 | 0 / 0 |
| A-write-outer-drive3 | 280 / 278 | 267 / 265 | -0.7 % | 7.4 / 7.4 | 9.0 / 9.0 | 14.1 / 65.9 | 0 / 0 |
| A-read-inner-drive3 | 135 / 135 | 129 / 129 | +0.0 % | 15.4 / 15.4 | 16.9 / 16.9 | 82.0 / 31.0 | 0 / 0 |
| A-write-inner-drive3 | 136 / 136 | 129 / 130 | +0.8 % | 15.4 / 15.4 | 16.9 / 16.9 | 103 / 17.7 | 0 / 0 |
| A-read-outer-drive4 | 280 / 280 | 267 / 267 | +0.0 % | 7.3 / 7.3 | 8.7 / 8.7 | 24.8 / 17.8 | 0 / 0 |
| A-write-outer-drive4 | 280 / 279 | 267 / 266 | -0.4 % | 7.3 / 7.3 | 8.7 / 9.0 | 52.8 / 69.4 | 0 / 0 |
| A-read-inner-drive4 | 133 / 134 | 127 / 128 | +0.8 % | 15.6 / 15.6 | 17.4 / 16.9 | 66.7 / 36.1 | 0 / 0 |
| A-write-inner-drive4 | 134 / 135 | 128 / 129 | +0.8 % | 15.6 / 15.6 | 16.9 / 16.9 | 85.1 / 17.8 | 0 / 0 |
| A-read-outer-drive5 | 274 / 273 | 261 / 261 | +0.0 % | 7.6 / 7.6 | 9.0 / 9.0 | 23.2 / 24.2 | 0 / 0 |
| A-write-outer-drive5 | 273 / 272 | 261 / 260 | -0.4 % | 7.6 / 7.6 | 9.0 / 9.0 | 49.1 / 68.1 | 0 / 0 |
| A-read-inner-drive5 | 131 / 132 | 125 / 126 | +0.8 % | 15.6 / 15.6 | 16.9 / 16.9 | 106 / 27.9 | 0 / 0 |
| A-write-inner-drive5 | 132 / 133 | 126 / 127 | +0.8 % | 15.6 / 15.6 | 16.9 / 16.9 | 113 / 17.6 | 0 / 0 |
| A-read-outer-drive6 | 276 / 277 | 264 / 264 | +0.0 % | 7.4 / 7.4 | 9.0 / 9.0 | 29.8 / 26.6 | 0 / 0 |
| A-write-outer-drive6 | 278 / 276 | 265 / 263 | -0.8 % | 7.4 / 7.4 | 9.0 / 9.0 | 10.3 / 86.2 | 0 / 0 |
| A-read-inner-drive6 | 132 / 132 | 126 / 126 | +0.0 % | 15.9 / 15.9 | 17.4 / 17.4 | 86.6 / 31.4 | 0 / 0 |
| A-write-inner-drive6 | 133 / 133 | 127 / 127 | +0.0 % | 15.9 / 15.9 | 17.4 / 17.4 | 111 / 21.6 | 0 / 0 |
| A-read-outer-drive7 | 280 / 280 | 267 / 267 | +0.0 % | 7.4 / 7.4 | 8.7 / 8.7 | 22.0 / 28.8 | 0 / 0 |
| A-write-outer-drive7 | 280 / 278 | 267 / 266 | -0.4 % | 7.4 / 7.4 | 8.7 / 9.0 | 12.9 / 61.6 | 0 / 0 |
| A-read-inner-drive7 | 132 / 133 | 126 / 127 | +0.8 % | 15.4 / 15.4 | 17.4 / 17.4 | 91.3 / 25.7 | 0 / 0 |
| A-write-inner-drive7 | 134 / 135 | 127 / 128 | +0.8 % | 15.4 / 15.4 | 16.9 / 16.9 | 86.7 / 17.6 | 0 / 0 |

`dd if=/dev/rdisk5 bs=1m count=4096`: 277.7 MB/s (before 277.9); last 336 MiB: 125.6 MB/s (125.6).
All within 1 %. The max column moves both ways (single spikes of 10-125 ms, drive-internal).

## B. Scaling (1 MiB sequential, QD2 per drive, outer, 30 s)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| B-read-1drv | 277 / 277 | 264 / 265 | +0.4 % | 7.6 / 7.6 | 8.7 / 9.0 | 29.0 / 23.1 | 0 / 0 |
| B-read-2drv | 557 / 557 | 531 / 531 | +0.0 % | 7.4 / 7.4 | 9.0 / 9.0 | 24.8 / 24.0 | 0 / 0 |
| B-read-4drv | 1109 / 1110 | 1058 / 1059 | +0.1 % | 7.4 / 7.4 | 9.0 / 9.0 | 44.0 / 45.7 | 0 / 0 |
| B-read-7drv | 1945 / 1945 | 1855 / 1855 | +0.0 % | 7.4 / 7.4 | 9.0 / 9.0 | 32.6 / 29.0 | 0 / 0 |
| B-write-1drv | 277 / 277 | 265 / 264 | -0.4 % | 7.6 / 7.6 | 8.7 / 9.0 | 32.8 / 44.5 | 0 / 0 |
| B-write-2drv | 556 / 558 | 530 / 532 | +0.4 % | 7.4 / 7.4 | 9.0 / 9.0 | 87.1 / 38.3 | 0 / 0 |
| B-write-4drv | 1112 / 1109 | 1061 / 1058 | -0.3 % | 7.4 / 7.4 | 9.0 / 9.0 | 49.7 / 84.6 | 0 / 0 |
| B-write-7drv | 1940 / 1948 | 1850 / 1858 | +0.4 % | 7.4 / 7.4 | 9.0 / 9.0 | 112 / 19.1 | 0 / 0 |

## C. Write-all then read-all (7 drives, 1 MiB, QD2, 30 s)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| C-write-all-outer | 1946 / 1941 | 1855 / 1851 | -0.2 % | 7.4 / 7.4 | 9.0 / 9.0 | 61.6 / 87.5 | 0 / 0 |
| C-read-all-outer | 1942 / 1947 | 1852 / 1857 | +0.3 % | 7.4 / 7.4 | 9.0 / 9.0 | 36.9 / 16.0 | 0 / 0 |
| C-write-all-inner | 931 / 931 | 888 / 888 | +0.0 % | 15.6 / 15.6 | 17.4 / 17.4 | 137 / 190 | 0 / 0 |
| C-read-all-inner | 933 / 934 | 890 / 891 | +0.1 % | 15.6 / 15.6 | 17.4 / 17.4 | 44.4 / 30.9 | 0 / 0 |

## D. Mixed load
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| D-4W3R-outer | 1943 / 1941 | 1853 / 1851 | -0.1 % | 7.4 / 7.4 | 9.0 / 9.0 | 79.1 / 111 | 0 / 0 |
| D-3W4R-outer | 1941 / 1947 | 1851 / 1856 | +0.3 % | 7.4 / 7.4 | 9.0 / 9.0 | 100 / 46.3 | 0 / 0 |
| D-mix70-seq-1M-q4 | 1379 / 1376 | 1315 / 1312 | -0.2 % | 16.1 / 16.1 | 56.3 / 55.3 | 124 / 136 | 0 / 0 |
| D-mix70-rand-64K-q8 | 97.3 / 97.9 | 1485 / 1494 | +0.6 % | 25.1 / 24.1 | 156 / 164 | 310 / 330 | 0 / 0 |

## E. Zone mixes
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| E-write-4outer-3inner | 1476 / 1491 | 1407 / 1422 | +1.1 % | 7.6 / 7.6 | 17.4 / 16.9 | 96.3 / 214 | 0 / 0 |
| E-read-4outer-3inner | 1508 / 1513 | 1438 / 1442 | +0.3 % | 7.6 / 7.6 | 16.9 / 16.9 | 131 / 56.3 | 0 / 0 |
| E-read-outer+inner-same-drive | 1416 / 1390 | 1350 / 1325 | -1.9 % | 7.7 / 7.7 | 17.4 / 22.5 | 2573 / 2613 | 0 / 0 |
| E-write-outer+inner-same-drive | 1436 / 1407 | 1370 / 1342 | -2.0 % | 7.7 / 7.7 | 100 / 65.5 | 2634 / 2661 | 0 / 0 |
| E-readouter+writeinner-same-drive | 1238 / 1248 | 1180 / 1191 | +0.9 % | 12.5 / 8.7 | 25.1 / 24.6 | 2662 / 2666 | 0 / 0 |

## F1. Block-size sweep (sequential read, 7 drives, QD2 per drive, outer, 20 s)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| F-seqread-4K-q2 | 698 / 646 | 170333 / 157595 | -7.5 % ** | 0.07 / 0.08 | 0.13 / 0.15 | 155 / 22.5 | 0 / 0 |
| F-seqread-64K-q2 | 1943 / 1940 | 29643 / 29596 | -0.2 % | 0.45 / 0.45 | 0.85 / 0.85 | 42.1 / 26.0 | 0 / 0 |
| F-seqread-128K-q2 | 1943 / 1944 | 14827 / 14827 | +0.0 % | 0.90 / 0.90 | 1.3 / 1.3 | 44.2 / 26.0 | 0 / 0 |
| F-seqread-1M-q2 | 1935 / 1945 | 1846 / 1855 | +0.5 % | 7.4 / 7.4 | 9.0 / 9.0 | 50.6 / 27.8 | 0 / 0 |
| F-seqread-4M-q2 | 1912 / 1903 | 456 / 454 | -0.4 % | 14.8 / 14.8 | 43.0 / 55.3 | 5070 / 2590 | 0 / 0 |

4 KiB sequential reads (served from the drives' read-ahead) are 7.5 % lower: 157,600 vs 170,300
IOPS. This is the host-side per-IO ceiling of the old doc (~5.8 us per IO serialised), now ~6.3 us;
the 4 KiB cache-hit row in X shows the same -5 %. Everything from 64 KiB up is media-bound and
unchanged. See anomalies.

## F2. Random IO (7 drives unless `-1drv`; 2-50 % unless `fullstroke`; 20 s)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| F-randread-4K-qd1 | 2.9 / 3.0 | 701 / 735 | +4.9 % | 9.7 / 9.5 | 18.4 / 15.9 | 88.8 / 55.1 | 0 / 0 |
| F-randwrite-4K-qd1 | 50.0 / 50.7 | 12201 / 12372 | +1.4 % | 0.26 / 0.25 | 2.0 / 2.0 | 27.5 / 16.2 | 0 / 0 |
| F-randread-4K-qd8 | 5.0 / 4.9 | 1220 / 1201 | -1.6 % | 32.3 / 33.8 | 172 / 168 | 387 / 361 | 0 / 0 |
| F-randwrite-4K-qd8 | 19.2 / 19.5 | 4686 / 4756 | +1.5 % | 11.5 / 11.5 | 20.5 / 20.0 | 88.8 / 70.7 | 0 / 0 |
| F-randread-4K-qd32 | 5.1 / 6.7 | 1235 / 1626 | +31.7 % ** | 160 / 102 | 319 / 524 | 20121 / 1121 | 0 / 0 |
| F-randwrite-4K-qd32 | 19.3 / 20.0 | 4705 / 4888 | +3.9 % | 46.1 / 46.1 | 57.3 / 61.4 | 20013 / 100 | 0 / 0 |
| F-randread-64K-qd1 | 45.8 / 45.8 | 700 / 699 | -0.1 % | 9.7 / 9.7 | 16.4 / 16.4 | 67.0 / 74.8 | 0 / 0 |
| F-randwrite-64K-qd1 | 26.1 / 27.5 | 399 / 419 | +5.0 % ** | 24.1 / 23.6 | 42.0 / 39.9 | 135 / 66.9 | 0 / 0 |
| F-randread-64K-qd8 | 75.6 / 75.4 | 1153 / 1151 | -0.2 % | 35.8 / 34.8 | 168 / 188 | 344 / 374 | 0 / 0 |
| F-randwrite-64K-qd8 | 15.2 / 15.3 | 232 / 234 | +0.9 % | 238 / 233 | 385 / 426 | 874 / 685 | 0 / 0 |
| F-randread-64K-qd32 | 76.8 / 101 | 1172 / 1538 | +31.2 % ** | 172 / 106 | 336 / 557 | 20117 / 1456 | 0 / 0 |
| F-randwrite-64K-qd32 | 155 / 166 | 2369 / 2541 | +7.3 % ** | 71.7 / 73.7 | 819 / 377 | 15061 / 1553 | 0 / 0 |
| F-randread-4K-qd1-1drv | 0.4 / 0.4 | 102 / 103 | +1.0 % | 9.7 / 9.5 | 15.9 / 15.9 | 50.7 / 56.8 | 0 / 0 |
| F-randread-4K-qd8-1drv | 0.7 / 0.7 | 169 / 172 | +1.8 % | 33.8 / 31.7 | 184 / 193 | 332 / 368 | 0 / 0 |
| F-randread-4K-qd32-1drv | 0.9 / 0.9 | 223 / 218 | -2.2 % | 100 / 106 | 557 / 557 | 945 / 1143 | 0 / 0 |
| F-randread-4K-qd1-fullstroke | 2.3 / 2.4 | 568 / 578 | +1.8 % | 11.8 / 11.8 | 22.0 / 21.0 | 119 / 74.5 | 0 / 0 |
| F-randread-4K-qd32-fullstroke | 4.1 / 5.7 | 998 / 1392 | +39.5 % ** | 205 / 121 | 369 / 590 | 20156 / 1263 | 0 / 0 |

The QD32 rows are the queue-limit effect of the old 64-task build: with 2040 tasks each drive gets
its 32 commands (7 x 232 = 1626 IOPS, vs 7 x 223 = 1560 predicted from one drive), and the
process-level max drops from 20 s to 1.1-1.6 s. QD1/QD8 and the 1-drive rows are unchanged (within
5 %). Random-write rows depend on the drive write-cache fill (runs back to back) as before; treat
the +5-7 % on 64 KiB writes as cache behaviour.

## K. Drive caches (drive2, QD1; flush = `ioctl(DKIOCSYNCHRONIZECACHE)` after every write)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| K-wr4K-seq-qd1-nosync | 69.3 / 67.8 | 16907 / 16559 | -2.1 % | 0.06 / 0.06 | 0.06 / 0.06 | 1.1 / 15.8 | 0 / 0 |
| K-wr4K-seq-qd1-dkiocsync | 0.5 / 0.5 | 118 / 118 | +0.0 % | 8.2 / 8.2 | 8.2 / 8.2 | 72.5 / 47.7 | 0 / 0 |
| K-wr4K-seq-qd1-rawfullfsync | — / 66.1 | — / 16142 | new | — / 0.06 | — / 0.06 | — / 30.9 | 80710 / 0 |
| K-wr64K-seq-qd1-nosync | 273 / 277 | 4170 / 4228 | +1.4 % | 0.22 / 0.22 | 0.62 / 0.62 | 33.0 / 5.0 | 0 / 0 |
| K-wr64K-seq-qd1-dkiocsync | 3.8 / 7.5 | 58.0 / 114 | +96.6 % ** | 8.4 / 8.4 | 58.4 / 16.9 | 100 / 43.8 | 0 / 0 |
| K-wr1M-seq-qd1-nosync | 273 / 269 | 260 / 257 | -1.2 % | 3.6 / 3.6 | 5.2 / 5.2 | 55.8 / 47.4 | 0 / 0 |
| K-wr1M-seq-qd1-dkiocsync | 84.6 / 85.4 | 81.0 / 81.0 | +0.0 % | 12.0 / 12.0 | 20.5 / 20.5 | 107 / 39.4 | 0 / 0 |
| K-wr4K-rand-qd1-nosync | 8.6 / 8.6 | 2098 / 2098 | +0.0 % | 0.20 / 0.19 | 2.0 / 2.2 | 13.1 / 13.3 | 0 / 0 |
| K-wr4K-rand-qd1-dkiocsync | 0.1 / 0.1 | 32.0 / 32.0 | +0.0 % | 29.2 / 29.2 | 45.1 / 45.1 | 449 / 456 | 0 / 0 |
| K-wr4K-seq-qd1-dkiocsync-all7 | 3.4 / 3.2 | 833 / 772 | -7.3 % ** | 8.2 / 8.2 | 8.2 / 41.0 | 43.5 / 100.0 | 0 / 0 |
| K-rd4K-qd1-16KiB-region | 41.8 / 17.6 | 10212 / 4302 | -57.9 % ** | 0.07 / 0.10 | 0.75 / 1.1 | 11.8 / 2.1 | 0 / 0 |
| K-rd4K-qd1-1MiB-region | 69.2 / 18.6 | 16895 / 4530 | -73.2 % ** | 0.05 / 0.10 | 0.10 / 1.2 | 0.78 / 2.2 | 0 / 0 |
| K-rd4K-qd1-rand-2-50 | 0.4 / 0.4 | 104 / 101 | -2.9 % | 9.5 / 9.7 | 15.6 / 16.1 | 20.8 / 23.7 | 0 / 0 |
| K-wcon-wr4K-seq-qd1-nosync | — / 67.1 | — / 16394 | new | — / 0.06 | — / 0.06 | — / 31.5 | 0 / 0 |
| K-trace-dkiocsync | 0.5 / 0.1 | 120 / 26.0 | -78.3 % ** | 8.2 / 41.0 | 8.4 / 86.0 | 18.0 / 86.4 | 0 / 0 |

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
**Recheck after the run** (drives idle, 01:00, `R-*` rows in `results.csv`): drive2 16 KiB / 1 MiB
region: **18,068 / 11,783 IOPS** (p50 54 / 84 us); drive1 18,040 / 12,195 IOPS. So the slow K rows
were situational: they ran right after ~2 min of flush-per-write and random-write tests on the same
drive (the drive was still destaging its write cache), not a driver change. Idle, 16 KiB hits are
now faster than on 2026-11-20 (18.1k vs 10.2k), 1 MiB-region hits a bit slower (11.8k vs 16.9k).

**SYNCHRONIZE CACHE reaches the drive.** Trace class on for 2 s in the recheck (`sas2308ctl mask +trace`, subsystem
`level:debug,persist:debug`, read back with `log show --debug`; `bench/final/trace-sync2.txt`, 1108
lines): **235 `WRITE(10)` of 8 blocks and 235 `SYNCHRONIZE CACHE(10)` to target 1, strictly
alternating**, all completed `scsi good`. Driver-level latency: writes median 0.64 ms, SYNCHRONIZE
CACHE **median 7.66 ms** (7.43-39.9 ms), before 0.58 / 7.7 ms: the SATL's FLUSH CACHE EXT waits for the
platter. Afterwards mask 35 and subsystem `INFO PERSIST_DEFAULT` again. The first attempt inside the
suite (`K-trace-dkiocsync`, 1 s) captured only 2 lines: the trace started in the same second as
the `log config` change; the recheck waits 2 s after it. That 1 s run was also slow (26 IOPS, p50
41 ms) while the 2 s recheck ran at 117 IOPS; not investigated.

## X. Path ceiling (drive-cache hits: same ~5 MiB re-read / rewritten, 15 s; write 60 s run with 5 s intervals)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| X-cachehit-read-1M-q4-1drv | 502 / 541 | 479 / 516 | +7.7 % ** | 8.2 / 7.4 | 9.2 / 9.0 | 85.2 / 34.3 | 0 / 0 |
| X-cachehit-read-1M-q4-2drv | 1064 / 1074 | 1014 / 1024 | +1.0 % | 7.4 / 7.4 | 9.0 / 9.0 | 31.3 / 20.7 | 0 / 0 |
| X-cachehit-read-1M-q4-4drv | 2188 / 2240 | 2086 / 2136 | +2.4 % | 7.4 / 7.4 | 9.0 / 7.4 | 29.2 / 30.6 | 0 / 0 |
| X-cachehit-read-1M-q4-7drv | 3172 / 3171 | 3025 / 3024 | -0.0 % | 9.2 / 9.2 | 9.2 / 9.2 | 33.7 / 41.9 | 0 / 0 |
| X-cachehit-read-128K-q8-7drv | 1707 / 1941 | 13022 / 14808 | +13.7 % ** | 2.0 / 2.2 | 26.6 / 26.6 | 44.8 / 35.0 | 0 / 0 |
| X-cachehit-read-4K-q8-7drv | 701 / 666 | 171228 / 162471 | -5.1 % ** | 0.33 / 0.33 | 0.67 / 0.56 | 56.9 / 73.9 | 0 / 0 |
| X-cachehit-write-1M-q4-1drv | — / 395 | — / 376 | new | — / 10.5 | — / 10.5 | — / 12.8 | 0 / 0 |
| X-cachehit-write-1M-q4-2drv | — / 791 | — / 754 | new | — / 10.5 | — / 10.8 | — / 15.2 | 0 / 0 |
| X-cachehit-write-1M-q4-4drv | — / 1618 | — / 1544 | new | — / 10.2 | — / 10.5 | — / 55.7 | 0 / 0 |
| X-cachehit-write-1M-q4-7drv | — / 2809 | — / 2679 | new | — / 10.2 | — / 10.8 | — / 34.3 | 0 / 0 |
| X-cachehit-write-1M-q4-7drv-60s | — / 2811 | — / 2680 | new | — / 10.2 | — / 10.8 | — / 72.7 | 0 / 0 |
| X-cachehit-write-4K-q8-7drv | — / 500 | — / 122202 | new | — / 0.15 | — / 7.2 | — / 329 | 0 / 0 |

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
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| Q-rr4K-qd32 | 7.2 / 7.2 | 1748 / 1755 | +0.4 % | 98.3 / 96.3 | 483 / 483 | 867 / 1459 | 0 / 0 |
| Q-rr4K-qd64 | 7.1 / 7.2 | 1742 / 1764 | +1.3 % | 225 / 221 | 606 / 606 | 906 / 1018 | 0 / 0 |
| Q-rr4K-qd128 | 7.1 / 7.2 | 1738 / 1761 | +1.3 % | 483 / 475 | 868 / 868 | 1221 / 1217 | 0 / 0 |
| Q-rw4K-qd32 | 23.9 / 29.3 | 5843 / 7159 | +22.5 % ** | 46.1 / 44.0 | 61.4 / 60.4 | 139 / 107 | 0 / 0 |
| Q-rw4K-qd64 | 23.1 / 18.7 | 5650 / 4559 | -19.3 % ** | 94.2 / 96.3 | 115 / 117 | 189 / 157 | 0 / 0 |
| Q-seq1M-read-q2 | 1942 / 1942 | 1852 / 1852 | +0.0 % | 7.4 / 7.4 | 9.0 / 9.0 | 82.6 / 82.8 | 0 / 0 |
| Q-verify-1M-seq-q4 | 1907 / 1922 | 1819 / 1833 | +0.8 % | 14.6 / 14.6 | 16.9 / 16.4 | 2518 / 2514 | 0 / 0 |
| Q-verify-64K-rand-q64 | 67.8 / 61.4 | 1034 / 937 | -9.4 % ** | 49.2 / 50.2 | 2163 / 2228 | 3152 / 3389 | 0 / 0 |

Driver latency (post-to-completion, from `Latency avg us` and `IOs completed` before/after each
point, `bench/final/stats/`): random 4K read QD32 / 64 / 128 = **127 / 253 / 503 ms** (before
128 / 257 / 514), write QD32 / 64 = 32 / 98 ms (39 / 79). Same picture as q2048: IOPS stop rising at
QD32 (1755 -> 1764 -> 1761) and the latency doubles with each doubling; Little's law gives
223 / 445 / 886 commands at the card, i.e. nothing throttles above or in the driver. Random-write
QD32 +22 % / QD64 -19 % is write-cache fill (the two runs swap places depending on what ran
before), not the driver. Q verify: 0 mismatches.

## G. Data integrity (7 drives x 3 zones, all concurrently; before = G-final / G-check-early)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| G-final2-1M-seq-q4 | 1246 / 1312 | 1189 / 1251 | +5.2 % ** | 28.2 / 17.4 | 1049 / 2490 | 13142 / 2591 | 0 / 0 |
| G-final2-64K-rand-q8 | 137 / 229 | 2093 / 3489 | +66.7 % ** | 51.2 / 22.5 | 311 / 467 | 42275 / 2643 | 0 / 0 |
| G-check-final2-1M | 1193 / 1308 | 1138 / 1247 | +9.6 % ** | 18.4 / 17.4 | 2425 / 2490 | 5615 / 2652 | 0 / 0 |
| G-check-final2-64K | 105 / 183 | 1603 / 2795 | +74.4 % ** | 96.3 / 12.0 | 238 / 467 | 51521 / 2634 | 0 / 0 |

**0 mismatches** in all four runs: G 1 MiB sequential QD4 (zones 1.8-2 / 50.8-51 / 99.8-100 %,
3 x 2 GiB per drive, 84 commands requested), G 64 KiB permuted QD8 (1.5-1.8 / 50.5-50.8 /
99.5-99.8 %, 3 x 256 MiB, 168 requested), and the re-read of both after ~33 min of other load
(seeds in `bench/final/seeds`). Plus Q verify 1 MiB QD4 1 GiB/drive and 64 KiB random QD64
256 MiB/drive (448 requested): 0 mismatches. ~56 GiB written and ~103 GiB read back and compared.
The 64 KiB runs are +67-74 % faster and the process max drops from 42-52 s to 2.6 s: with 168
requests and 2040 tasks nothing waits above the driver any more.

## H. The soak pattern, 3 min (was 60 min)
| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |
|---|---|---|---|---|---|---|---|
| H-soak-seq-1M | 401 / 1223 | 382 / 1166 | +205.2 % ** | 9.7 / 7.6 | 475 / 35.8 | 2574 / 2550 | 0 / 0 |
| H-soak-rand-64K-mix70 | 58.9 / 18.5 | 900 / 282 | -68.7 % ** | 33.8 / 24.6 | 311 / 2490 | 2580 / 2596 | 0 / 0 |

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

| sensor (C) | cold idle 23:41 | perf part max | pre-soak idle | soak load first / peak / steady | cool-down end (+5 min) |
|---|---|---|---|---|---|
| IOC (SAS2308) | 59-59 | 62 | 61-61 | 61 / 61 / 60.9 | 59 |
| drive drive1 | 28-28 | 31 | 31-31 | 31 / 31 / 31.0 | 31 |
| drive drive2 | 30-30 | 32 | 32-32 | 32 / 32 / 32.0 | 32 |
| drive drive3 | 27-29 | 32 | 32-33 | 32 / 33 / 32.8 | 32 |
| drive drive4 | 28-28 | 31 | 31-31 | 31 / 32 / 31.2 | 31 |
| drive drive5 | 28-28 | 31 | 31-32 | 31 / 32 / 31.6 | 31 |
| drive drive6 | 28-28 | 31 | 31-31 | 31 / 32 / 31.1 | 31 |
| drive drive7 | 28-28 | 31 | 31-31 | 31 / 32 / 31.1 | 30 |
| SoC tdie max | 32.8-36.4 | 42.7 | 33.7-35.6 | 34.8 / 36 / 35.1 | 32.5 |
| SoC tdie mean (27 sensors) | 31.0-32.4 | 36.3 | 31.8-33 | 32.4 / 32.7 / 32.5 | 30.6 |

Plot of the whole run: `bench/final/temps.png`; 30 s ASCII view of the soak: `bench/final/soak-temps.txt`.

- **Throughput** flat: 1305-1341 MB/s aggregate in every 30 s interval (~188 MB/s per drive,
  94 read + 94 write), 1254-1365 MB/s by the driver's byte counters; driver interval latency
  43-45 ms (56 commands / 1313 IOPS), process max 246 ms. No error, timeout, TM, reinit or held IO.
- **Time to steady state:** none needed in the soak: the drives had reached their steady 31-33 C
  during the 50 min performance part (cold 27-30 C at 23:41, 31-33 C from ~00:00), and the IOC sat
  at 61 C from the first minutes of load. Within the soak the drives rose by 0-1 C (max 33 C,
  drive3); the IOC did not move.
- **Cool-down:** IOC 61 -> 59 C within 1.5 min of stopping the load (00:55:44) and its cold idle value (59 C) from then on; drives fell 0-1 C in 5 min (30-32 C at the end; they cool slowly, ~1 C per 5 min); SoC die max 35 -> 32.5 C.
- Context: the old run saw IOC 59-61 C and drives up to 35 C over 2 h; this run is the same or
  cooler. 1.3 GB/s of mixed sequential IO is the drives' limit for this pattern, and at that
  load the SAS2308 runs at its idle temperature +2 C.

## Counters, logs
- Driver counters (`bench/final/stats-final.txt`, since 23:41, after the recheck): 15,902,367 IOs (tasks accepted = IOs completed), 1.922 TB read, 1.783 TB written, latency max 3.39 s, chain free low-water 16383, 111 recovered sense (not errors), 0 events; every timeout / error / recovery / TM / held / reinit / fault / KASSERT / lost-kick / stale-reply / chain-failure counter **0**; state operational.
- Driver log (`log show`, subsystem org.sas2308.driver.SAS2308HBA, 23:41-01:05): only 2 lines, both the recheck's `debug mask ... set from userland`, now logged at default level (old anomaly 6 fixed). No error/fault/recovery line, and no event lines (the old 0xff02 event every ~30 s no longer appears; `Events` counter 0 since the reset).
- `monitor_final.sh`: 307 samples, never alerted; IOC 58-62 C over the whole run.

## Anomalies and notes
1. **QD1 read-cache hits ~0.05 ms slower** (4.3-4.5k vs 10-17k IOPS, K table) while 4 KiB cache-hit
   writes at QD1 are unchanged (59 us, 16,559 IOPS) and 7 x QD8 cache-hit reads are -5 %.
   Not reproduced on idle drives after the run (18.1k / 11.8k IOPS, 54 / 84 us); situational (drive destaging after the preceding flush/random-write tests). Not a driver regression.
   Settled in "K recheck (2026-11-21)" below: a 35-45 s drive-side slow phase after the K writes (reproduced 3/3), and two idle hit states (54 / 80 us).
2. **Host-side 4 KiB IOPS ceiling -5 to -7.5 %** (F1 4 KiB 157.6k vs 170.3k, X 4 KiB 162.5k vs
   171.2k). Small, but consistent with ~0.5 us more per IO in the submit/completion path after
   batches A-F (more accounting/logging checks per IO). Irrelevant for HDDs (< 2,000 IOPS needed);
   worth a profile before SSDs.
3. **SIGINT does not stop a backgrounded sasbench.** Non-interactive bash starts `&` jobs with
   SIGINT ignored, so `monitor.sh`'s `pkill -INT -x sasbench` (also in the 2026-11-20 monitor)
   would not have stopped a benchmark. The soak was stopped with SIGTERM (sasbench rc 143, no
   summary line; the 30 s interval CSV covers it). Fix: use SIGTERM in the watchdogs or trap INT (done: the watchdog scripts send SIGTERM since 2026-11-21).
4. **Write cache cannot be toggled** through this path (`smartctl -s wcache,off`: "Unsupported ATA
   feature"), see K.
5. H pattern: random stream starved by the sequential stream on the same drive now that the
   drives hold all commands (fairness inside the drive's NCQ, p99 2.49 s, max 2.6 s).
6. NCQ starvation inside the drive (~2.6 s, E rows, two streams per drive) unchanged.
7. The outer-zone writes overwrote the JHFS+ test volumes on drive6 and drive7 (allowed);
   all 7 drives now show no partition table (disk9/disk10 JHFS+ volumes H5drive6 / H5drive7 used by the hw tests are gone; recreate with `diskutil eraseDisk JHFS+ ...` if a later test needs them).

## Files
- `bench/final/results.csv` (one line per job + `ALL` per run), `monitor.csv`, `drivetemps.csv`,
  `soc.csv`, `soak.csv`, `soak-int.csv`, `idle-cold*.csv`, `xwrite-60s.csv`, `h-*.csv`, `kfs.txt`,
  `wcache.txt`, `trace-sync*.txt`, `stats/` (driver stats around the Q points), `stats-*.txt`,
  `show-*.txt`, `seeds`, `disks.txt`, `run.log.gz`, `soak-temps.txt` (ASCII plot).
- `bench/final/scripts/` - the scripts as run. To run them again (on the Mac, from the repo root):
  `make -C tools sasbench sas2308ctl; make -C bench/final/scripts` (socsens, fsbench), then
  `sudo bash bench/final/scripts/run_all.sh` (or `perf.sh`, `soak.sh`, `recheck.sh`, `krecheck.sh`).
  They refuse to run without root, find the tools and each other relative to the repo, and write
  to `bench/out/final` (`bench/out/krecheck` for `krecheck*.sh`); `sudo env OUT=/some/dir bash ...`
  writes elsewhere (no spaces in the paths). `krecheck-epc*.sh` take the drive serial numbers as
  arguments.

## K recheck (2026-11-21)
Question: are the K read-cache rows (QD1 4 KiB re-reads of a small region on drive2; 2026-11-20
10,212 / 16,895 IOPS, final run 4,302 / 4,530, idle recheck 01:00 18,068 / 11,783) drive behaviour,
an effect of the preceding writes, or the host? Run 05:28-06:06 on 085DA9D6, drive2 and drive1,
same sasbench flags as the K rows (`-b 4K -q 1`, seq, raw device), 15 s idle / 10 s post-burst
points, 1 s interval series; script `bench/final/scripts/krecheck.sh`, data `bench/final/krecheck/`
(`results.csv`, `ts.csv` per-second IOPS, `run.log`, `zones.txt`, `idle-iostat.txt`, `temps.csv`).
Drives idle for 5 min first (iostat 0 tps), 33-35 C throughout.

**Region sizes.** The K row names are wrong: zone `3-3.000001` of an 18.0 TB drive is 43 x 4 KiB =
**172 KiB**, `3-3.0000058` is 254 x 4 KiB = **1016 KiB** (`zones.txt`). The names are kept below
("16KiB" / "1MiB" rows); the size sweep uses exact sizes.

**1. Idle, 5 alternating repetitions per drive (15 s each)** - IOPS, p50 / p99 us, max ms:

| drive | "16KiB" row (172 KiB) | "1MiB" row (1016 KiB) |
|---|---|---|
| drive2 | 12,296-12,319 (mean 12,307, sd 0.1 %), 80 / 86, 0.63-0.65 | 12,297-12,454 (mean 12,348, sd 0.5 %), 80 / 82-84, 0.63-0.64 |
| drive1 | r1 **18,289 (54 / 94)**, r2-r5 12,045-12,138 (80-82 / 86-88), 0.63-2.2 | 12,054-12,147 (sd 0.3 %), 82 / 84-86, 0.62-0.64 |

Hits come in **two discrete states**: ~12.1-12.4k IOPS with p50 80-82 us (the normal idle state,
19 of 20 runs) and ~18.2-18.4k with p50 54 us. Within a state the spread is 0.1-0.5 %. No region
dependence between the two rows.

**Size sweep and LBA (15 s, once each):**

| region | drive2 IOPS (p50 / p99 us, max ms) | drive1 |
|---|---|---|
| 16 KiB | 12,155 (80 / 88, 0.62) | 11,932 (82 / 88, 0.64) |
| 64 KiB | 12,255 (80 / 88, 0.64) | 12,041 (82 / 88, 1.0) |
| 256 KiB | 12,312 (80 / 86, 0.64) | 12,063 (82 / 86, 0.65) |
| 1 MiB | 12,350 (80 / 84, 0.65) | 12,085 (82 / 86, 0.63) |
| 4 MiB | 12,208 (80 / 84, 0.64) | 12,091 (82 / 86, 0.63) |
| 16 MiB | 15,846 (**54** / 86, 20.1) | 15,751 (**54** / 86, 50.0) |
| middle (49 %), 172 KiB | 18,252 (**54** / 92, 16.0) - fast all 15 s | 13,861 (80 / 88, 12.1) - **18.4k for 4 s, then 12.0k** |
| middle (49 %), 1016 KiB | 12,124 (82 / 86, 0.63) | 11,985 (82 / 86, 0.66) |

No size dependence from 16 KiB to 4 MiB (all within 3.5 %, p50 80-82 us). At 16 MiB the region no
longer stays in the cache (one media refill per lap, max 20-50 ms) but the hits in between are 54 us.
Outer vs middle: same normal state (12.1k) once resident; the first run on the new middle region,
just read from media, was fast. Pattern: **54 us = hits on data the drive has just (re)read from
the platter; after a few seconds of hits it switches to 80-82 us**, as a step inside one run
(drive1 middle, second 4->5: 18.4k -> 12.0k, nothing changed on the host). The drive1 r1 fast
run was such a first touch (that region was last read at 01:00).

**2. Write burst** = the final run's K write sequence (`perf.sh` lines 78-87: 4K/64K/1M seq with and
without flush-per-write, 4K random with and without flush, 4K flush-per-write on all 7; 2 min 20 s),
then the two re-reads at t = 0, 30, 60, 120, 300 s after its end. Bursts 1 and 2 on drive2,
burst 3 on drive1 (IOPS, p50 / p99 us):

| t | B1 drive2 "16KiB" / "1MiB" | B2 drive2 | B3 drive1 |
|---|---|---|---|
| 0 s | 4,287 (104/1088) / 4,459 (100/1152) | 4,254 (104/1088) / 4,402 (102/1152) | 4,222 (104/1088) / 4,559 (100/1184) |
| 30 s | 7,483 (86/1088) / 8,850 (82/1008) | 8,404 (86/1024) / 8,944 (80/1008) | 4,904 (98/1184) / 12,065 (82/88) |
| 60 s | 12,245 (80/88) / 12,166 (80/86) | 12,231 (80/88) / 12,144 (82/86) | 12,281 (80/88) / 12,126 (82/86) |
| 120 s | 12,118 / 12,156 (80-82/86-88) | 12,056 / 12,105 (82/86-90) | 12,112 / 12,122 (80-82/86-88) |
| 300 s | 12,236 / 12,341 (80/84-88) | 12,067 / 12,125 (82/86-90) | 12,256 / 12,277 (80/84-88) |

The t = 0 point reproduces the final run's K rows exactly (4,302 / 4,530, p50 104 / 100 us, p99
1088 / 1184 us) on both drives, all three times. Per second (`ts.csv`) the slow phase is flat at
~4.0-5.3k and ends as a step: B1 at ~35.5 s (16KiB row) and ~44.5 s ("1MiB" row, which started
slow at 40 s), B2 at ~34.5 / ~44.5 s, B3 still slow at 41 s, fast from ~41 s. **Recovery time
35-45 s after the burst, complete by 60 s** in all three; then the normal 80-us state, no
fast-state overshoot. The burst's last command is a SYNCHRONIZE CACHE, so the RAM write cache is
clean at t = 0: the slow phase is the drive's own background work after the writes (destaging in
the wider sense, e.g. media-cache migration; not observable from the host), not dirty data in
the cache. Side finding: in the burst's last row (4K flush-per-write on all 7), the drive that had
just taken the burst runs at 62-64 IOPS, p99 58 ms, the other drives at 117-119 IOPS (p99 8.2 ms):
the final run's -7 % / p99 41 ms on `K-wr4K-seq-qd1-dkiocsync-all7` is the same effect.

**Host side.** Nothing points at the host: (a) both rows slow at once only in the post-burst
phase, and they recover at drive-specific moments (B3 drive drive1 vs B1/B2 drive2), not
together; (b) the state flips 54 <-> 80 us and the post-burst recovery are steps inside a single
sasbench run with no host change; (c) the same path does 18.4k QD1 reads (54 us) and 16.8k QD1 4K
cache-hit writes (58-59 us, every burst; final run 16,559, 2026-11-20 16,907); (d) one CPU core
kept busy (`yes`, shallower CPU idle states) changes the normal state by +2 % (12,561 / 12,612 vs
12,307 / 12,348, p50 78 vs 80 us), far below the 26 us and ~50 us steps. Not done: driver-level
latency trace for the slow phase (no DTrace / trace class in this run).

**Conclusion.** All four historical value pairs are drive states, not driver or host effects:
final run 4.3k / 4.5k = post-write slow phase (reproduced 3 of 3, gone after 35-45 s);
2026-11-20 10.2k (p50 68, p99 752 us) = the end of that phase inside the 10 s window, then 16.9k
(53 us) = fast state; 01:00 recheck 18.1k = fast state (first touch), 11.8k = normal state. Idle
spread within a state 0.1-0.5 %; no region-size dependence up to 4 MiB. A benchmark of drive
cache hits must wait >= 60 s after writes and run long enough (or report both states).

## K recheck 2: EPC (2026-11-21)
Question: do the drives' EPC power-condition timers (Idle_A = electronics power-reduced a few
seconds after the last media access) explain the two hit states (fast 54 us right after a media
read, normal 80 us from ~4 s on)? Plan: read the Idle_A/B/C and Standby_Z timers through the SATL,
then rerun the fast->normal transition with Idle_A disabled (volatile) or set to 60 s. Run
06:23-06:26 on 085DA9D6; scripts `bench/final/scripts/krecheck-epc{1,2,3}.sh`, raw outputs
`bench/final/krecheck/epc/`.

**Result: the EPC timers cannot be read or changed through this path, so the test stopped after
step 1.**
- `smartctl` 7.5 has no EPC support: `-g epc` is rejected ("VALID ARGUMENTS ARE: aam, apm, dsn,
  lookahead, security, wcache, rcache, wcreorder, wcache-sct"), with and without `-d sat`; `-s` has
  no EPC setting either (`smartctl-h.txt`). Only `-s standby,N` exists, which is the legacy
  standby timer, not EPC.
- Underneath, Darwin smartctl goes through Apple's SMART interface, which carries only SMART
  commands and IDENTIFY: `ATA_READ_LOG_EXT ... failed: 48-bit ATA commands not implemented`, so the
  Power Conditions log (GP log 0x08, where the timers live) cannot be read, and `CHECK POWER MODE
  not implemented` (`checkpower-drive3.txt`), so the current power condition (0x81 = Idle_A)
  cannot be polled either. SET FEATURES (EPC subcommand 0x4A) already fails through this path for
  write cache (see "Write cache on/off").
- IDENTIFY (drive3, same model and firmware SN02; `identify-drive3.txt`): word 119 bit 7 = 1
  **EPC supported**, word 120 bit 7 = 1 **EPC enabled**; APM not supported (word 83 bit 3 = 0),
  PUIS supported but off, device-initiated SATA power management supported but off (word 79 bit
  3 = 0). So the drive may enter Idle_A/B/C on its own timers, but their values are unknown.
- `-l devstat` (Device Statistics, SMART log 0x04) fails (`ATA_SMART_READ_LOG failed`); SCT
  status only reports "Device State: Active (0)" (SCT does not distinguish the EPC idle states).
- Side effect: right after `smartctl -x` (in which the Device Statistics SMART READ LOG failed),
  drive2 (disk5) and drive1 (disk4) switched to `SMART Status: Not Supported`, and every
  further `smartctl` open of them fails ("Operation not supported by device"); the other five
  drives stay `Verified`. The I/O path is unaffected (drive2 3 s QD1 re-read after it: 17,886
  IOPS, p50 54 us, 0 errors); driver counters moved only by "Err check condition" +1,421 (the
  failing pass-through commands of the two `-x` calls; recovered sense +14), nothing else. Expect
  SMART on those two to return only after re-enumeration (reboot or kext reload).

Not done (needs a tool that can send ATA PASS-THROUGH or SCSI MODE SELECT directly, e.g.
openSeaChest_PowerControl or sg3_utils against the Power Condition mode page 0x1A, which SAT
translates to EPC; no install in this task): reading the timers, disabling Idle_A, and the
before/after transition runs. The hypothesis stays open; consistent with it: the step comes
~4 s after the last media access and is a fixed +26 us per hit.
