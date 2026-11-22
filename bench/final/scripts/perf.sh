#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# Final performance suite (run with sudo). Every pattern of docs/benchmarks-20261120.md, shorter points.
. "$(dirname "$0")/lib_final.sh"
SC=${SMARTCTL:-$(command -v smartctl || echo smartctl)}
echo "PERF START $(date '+%F %T')" >> $LOG
setup || exit 1
# idle baseline (cold), 2 min
for k in 1 2 3 4 5; do
  echo "$(date +%T),$($CTL stats | awk '/IOC temperature \(C\)/ {print $NF}'),$($SOC -s | tr ' ' ',')" >> $OUT/idle-cold.csv
  for d in "${D[@]}"; do echo "$(date +%T),$d,$($SC -A /dev/$d | awk '$1==194 {print $10; exit}')" >> $OUT/idle-cold-drives.csv; done
  [ $k -lt 5 ] && sleep 30
done
touch $OUT/abort_on_alert

## KFS: file system F_FULLFSYNC on the JHFS+ test volume of drive index 5 (before raw writes clobber it)
setup || exit 1
PART=${D[5]}s2
if diskutil info $PART 2>/dev/null | grep -q "Journaled HFS+" && diskutil mount $PART >> $LOG 2>&1; then
  MP=$(diskutil info $PART | awk -F': *' '/Mount Point/ {print $2}')
  echo "KFS on $PART at $MP" >> $LOG
  for m in 0 1 2 3; do $FSB "$MP/fsbench.dat" 64 4096 15 $m KFS-4K-mode$m >> $OUT/kfs.txt 2>&1; done
  for m in 0 2; do $FSB "$MP/fsbench.dat" 64 1048576 15 $m KFS-1M-mode$m >> $OUT/kfs.txt 2>&1; done
  rm -f "$MP/fsbench.dat"; diskutil unmount $PART >> $LOG 2>&1
else echo "KFS skipped: $PART not JHFS+ or mount failed" >> $LOG; fi

## A: per drive
setup || exit 1; warm A
for i in $ALL; do
  run "A-read-outer-${SER[$i]}"  -b 1M -q 2 -t 15 $(jobs r outer $i)
  run "A-write-outer-${SER[$i]}" -b 1M -q 2 -t 15 $(jobs w outer $i)
  run "A-read-inner-${SER[$i]}"  -b 1M -q 2 -t 15 $(jobs r inner $i)
  run "A-write-inner-${SER[$i]}" -b 1M -q 2 -t 15 $(jobs w inner $i)
done
if ! stopped; then umount7
  echo "== dd cross-check ${D[1]} $(date +%T)" >> $LOG
  dd if=/dev/r${D[1]} of=/dev/null bs=1m count=4096 >> $LOG 2>&1
  dd if=/dev/r${D[1]} of=/dev/null bs=1m count=336 skip=17166000 >> $LOG 2>&1
fi

## G: integrity (written now, re-checked at the end)
setup || exit 1; warm G
SEED1=$(date +%s); SEED2=$((SEED1+1)); echo "final2 $SEED1 $SEED2" >> $OUT/seeds
G1Z="1.8-2 50.8-51 99.8-100"; G2Z="1.5-1.8 50.5-50.8 99.5-99.8"
j1=""; for z in $G1Z; do j1="$j1 $(jobs v $z $ALL)"; done
j2=""; for z in $G2Z; do j2="$j2 $(jobs v $z $ALL)"; done
run G-final2-1M-seq-q4   -b 1M  -q 4 -B 2G   -s $SEED1 $j1
run G-final2-64K-rand-q8 -b 64K -q 8 -B 256M -p rand -s $SEED2 $j2

## B..F
setup || exit 1; warm B
for set in "1" "1 2" "1 2 3 4" "$ALL"; do n=$(echo $set | wc -w | tr -d ' '); run "B-read-${n}drv"  -b 1M -q 2 -t 30 $(jobs r outer $set); done
for set in "1" "1 2" "1 2 3 4" "$ALL"; do n=$(echo $set | wc -w | tr -d ' '); run "B-write-${n}drv" -b 1M -q 2 -t 30 $(jobs w outer $set); done
run C-write-all-outer -b 1M -q 2 -t 30 $(jobs w outer $ALL)
run C-read-all-outer  -b 1M -q 2 -t 30 $(jobs r outer $ALL)
run C-write-all-inner -b 1M -q 2 -t 30 $(jobs w inner $ALL)
run C-read-all-inner  -b 1M -q 2 -t 30 $(jobs r inner $ALL)
setup || exit 1
run D-4W3R-outer -b 1M -q 2 -t 30 $(jobs w outer 0 1 2 3) $(jobs r outer 4 5 6)
run D-3W4R-outer -b 1M -q 2 -t 30 $(jobs w outer 0 1 2) $(jobs r outer 3 4 5 6)
run D-mix70-seq-1M-q4  -b 1M  -q 4 -t 30 -M 70 $(jobs m 0-1.5 $ALL)
run D-mix70-rand-64K-q8 -b 64K -q 8 -t 30 -M 70 -p rand $(jobs m 2-50 $ALL)
run E-write-4outer-3inner -b 1M -q 2 -t 30 $(jobs w outer 0 1 2 3) $(jobs w inner 4 5 6)
run E-read-4outer-3inner  -b 1M -q 2 -t 30 $(jobs r outer 0 1 2 3) $(jobs r inner 4 5 6)
run E-read-outer+inner-same-drive  -b 1M -q 2 -t 30 $(jobs r outer $ALL) $(jobs r inner $ALL)
run E-write-outer+inner-same-drive -b 1M -q 2 -t 30 $(jobs w outer $ALL) $(jobs w inner $ALL)
run E-readouter+writeinner-same-drive -b 1M -q 2 -t 30 $(jobs r outer $ALL) $(jobs w inner $ALL)
setup || exit 1
for bs in 4K 64K 128K 1M 4M; do run "F-seqread-$bs-q2" -b $bs -q 2 -t 20 $(jobs r outer $ALL); done
for bs in 4K 64K; do for qd in 1 8 32; do
  run "F-randread-$bs-qd$qd"  -b $bs -q $qd -t 20 -p rand $(jobs r 2-50 $ALL)
  run "F-randwrite-$bs-qd$qd" -b $bs -q $qd -t 20 -p rand $(jobs w 2-50 $ALL)
done; done
for qd in 1 8 32; do run "F-randread-4K-qd$qd-1drv" -b 4K -q $qd -t 20 -p rand $(jobs r 2-50 1); done
for qd in 1 32; do run "F-randread-4K-qd$qd-fullstroke" -b 4K -q $qd -t 20 -p rand $(jobs r full $ALL); done

## K: drive caches (drive index 1 = drive2, the same drive as on 2026-11-20)
setup || exit 1; warm K; i=1
run K-wr4K-seq-qd1-nosync     -b 4K  -q 1 -t 15 $(jobs w 3-3.1 $i)
run K-wr4K-seq-qd1-dkiocsync  -b 4K  -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
run K-wr4K-seq-qd1-rawfullfsync -b 4K -q 1 -t 5 -F 1 $(jobs w 3-3.1 $i)
run K-wr64K-seq-qd1-nosync    -b 64K -q 1 -t 15 $(jobs w 3-3.1 $i)
run K-wr64K-seq-qd1-dkiocsync -b 64K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
run K-wr1M-seq-qd1-nosync     -b 1M  -q 1 -t 15 $(jobs w 3-3.1 $i)
run K-wr1M-seq-qd1-dkiocsync  -b 1M  -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
run K-wr4K-rand-qd1-nosync    -b 4K  -q 1 -t 15 -p rand $(jobs w 2-50 $i)
run K-wr4K-rand-qd1-dkiocsync -b 4K  -q 1 -t 15 -p rand -F 2 $(jobs w 2-50 $i)
run K-wr4K-seq-qd1-dkiocsync-all7 -b 4K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $ALL)
run K-rd4K-qd1-16KiB-region -b 4K -q 1 -t 10 $(jobs r 3-3.000001 $i)
run K-rd4K-qd1-1MiB-region  -b 4K -q 1 -t 10 $(jobs r 3-3.0000058 $i)
run K-rd4K-qd1-rand-2-50    -b 4K -q 1 -t 10 -p rand $(jobs r 2-50 $i)
# write cache off (ATA SET FEATURES via smartctl), then back on
$SC -g wcache /dev/${D[$i]} | grep -i "write cache" > $OUT/wcache.txt 2>&1
if ! stopped && $SC -s wcache,off /dev/${D[$i]} >> $OUT/wcache.txt 2>&1 && $SC -g wcache /dev/${D[$i]} | grep -qi "write cache is: *disabled"; then
  echo "wcache off on ${D[$i]} $(date +%T)" >> $LOG
  run K-wcoff-wr4K-seq-qd1-nosync    -b 4K -q 1 -t 15 $(jobs w 3-3.1 $i)
  run K-wcoff-wr4K-seq-qd1-dkiocsync -b 4K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
  run K-wcoff-wr64K-seq-qd1-nosync   -b 64K -q 1 -t 15 $(jobs w 3-3.1 $i)
  run K-wcoff-wr1M-seq-qd1-nosync    -b 1M -q 1 -t 15 $(jobs w 3-3.1 $i)
  run K-wcoff-wr4K-rand-qd1-nosync   -b 4K -q 1 -t 15 -p rand $(jobs w 2-50 $i)
else echo "wcache off failed or not confirmed" >> $LOG; fi
$SC -s wcache,on /dev/${D[$i]} >> $OUT/wcache.txt 2>&1
$SC -g wcache /dev/${D[$i]} | grep -i "write cache" >> $OUT/wcache.txt 2>&1
echo "wcache after restore: $(tail -1 $OUT/wcache.txt)" >> $LOG
run K-wcon-wr4K-seq-qd1-nosync -b 4K -q 1 -t 10 $(jobs w 3-3.1 $i)
# SYNCHRONIZE CACHE trace: trace class + subsystem debug persist for ~1 s, read back with log show --debug
OLDM=$($CTL show | awk '/SAS2308DebugMask/ {print $NF; exit}')
T0=$(date '+%Y-%m-%d %H:%M:%S')
/usr/bin/log config --subsystem io.github.minhnhatnoe.sas2308 --mode "level:debug,persist:debug" >> $LOG 2>&1
$CTL mask +trace >> $LOG 2>&1
run K-trace-dkiocsync -b 4K -q 1 -t 1 -F 2 $(jobs w 3-3.1 $i)
$CTL mask $OLDM >> $LOG 2>&1
/usr/bin/log config --subsystem io.github.minhnhatnoe.sas2308 --mode "level:info,persist:default" >> $LOG 2>&1
echo "mask now $($CTL show | awk '/SAS2308DebugMask/ {print $NF; exit}') (was $OLDM); $(/usr/bin/log config --status --subsystem io.github.minhnhatnoe.sas2308)" >> $LOG
sleep 2
/usr/bin/log show --debug --style compact --start "$T0" --predicate 'subsystem == "io.github.minhnhatnoe.sas2308"' 2>/dev/null | grep -E "IO (start|done)" > $OUT/trace-sync.txt

## X: path ceiling, reads (drive read-cache hits) and writes (drive write cache absorbs a small region)
setup || exit 1
for set in "1" "1 2" "1 2 3 4" "$ALL"; do n=$(echo $set | wc -w | tr -d " "); run "X-cachehit-read-1M-q4-${n}drv" -b 1M -q 4 -t 15 $(jobs r 3-3.00003 $set); done
run X-cachehit-read-128K-q8-7drv -b 128K -q 8 -t 15 $(jobs r 3-3.00003 $ALL)
run X-cachehit-read-4K-q8-7drv   -b 4K -q 8 -t 15 $(jobs r 3-3.00003 $ALL)
for set in "1" "1 2" "1 2 3 4" "$ALL"; do n=$(echo $set | wc -w | tr -d " "); run "X-cachehit-write-1M-q4-${n}drv" -b 1M -q 4 -t 15 $(jobs w 3-3.00003 $set); done
run X-cachehit-write-1M-q4-7drv-60s -b 1M -q 4 -t 60 -T $OUT/xwrite-60s.csv -i 5 $(jobs w 3-3.00003 $ALL)
run X-cachehit-write-4K-q8-7drv -b 4K -q 8 -t 15 $(jobs w 3-3.00003 $ALL)

## Q: queue-depth table (60 s per point), driver stats around each point
setup || exit 1; SNAP=1
run Q-rr4K-qd32  -b 4K -q 32  -t 60 -p rand $(jobs r 2-50 $ALL)
run Q-rr4K-qd64  -b 4K -q 64  -t 60 -p rand $(jobs r 2-50 $ALL)
run Q-rr4K-qd128 -b 4K -q 128 -t 60 -p rand $(jobs r 2-50 $ALL)
run Q-rw4K-qd32  -b 4K -q 32  -t 60 -p rand $(jobs w 2-50 $ALL)
run Q-rw4K-qd64  -b 4K -q 64  -t 60 -p rand $(jobs w 2-50 $ALL)
run Q-seq1M-read-q2 -b 1M -q 2 -t 60 $(jobs r outer $ALL)
QS=$(date +%s); echo "Q $QS $((QS+1))" >> $OUT/seeds
run Q-verify-1M-seq-q4   -b 1M  -q 4  -B 1G   -s $QS $(jobs v 1.2-1.3 $ALL)
run Q-verify-64K-rand-q64 -b 64K -q 64 -B 256M -p rand -s $((QS+1)) $(jobs v 1.3-1.4 $ALL)
SNAP=

## G-check: re-read the G regions after all the other load
setup || exit 1
j1=""; for z in $G1Z; do j1="$j1 $(jobs c $z $ALL)"; done
j2=""; for z in $G2Z; do j2="$j2 $(jobs c $z $ALL)"; done
run G-check-final2-1M  -b 1M  -q 4 -B 2G   -s $SEED1 $j1
run G-check-final2-64K -b 64K -q 8 -B 256M -p rand -s $SEED2 $j2

## H: the soak pattern, 3 min
setup || exit 1; warm H
echo "== H soak 3 min $(date +%T)" >> $LOG
if ! stopped; then
  $SB -o $R -l H-soak-seq-1M -b 1M -q 2 -t 180 -T $OUT/h-seq.csv -i 30 $(jobs w 0-1.5 0 1 2 3) $(jobs r 98-99.5 4 5 6) > $OUT/h-seq.txt 2>&1 &
  P1=$!
  $SB -o $R -l H-soak-rand-64K-mix70 -b 64K -q 8 -M 70 -p rand -t 180 -T $OUT/h-rand.csv -i 30 $(jobs m 2-50 $ALL) > $OUT/h-rand.txt 2>&1 &
  P2=$!; wait $P1; r1=$?; wait $P2; r2=$?
  echo "H rc seq=$r1 rand=$r2 $(date +%T)" >> $LOG
fi
rm -f $OUT/abort_on_alert
$CTL stats > $OUT/stats-perf-end.txt 2>&1
echo "PERF DONE $(date '+%F %T')" >> $LOG
