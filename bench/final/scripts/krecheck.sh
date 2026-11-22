#!/bin/bash
# K recheck (run with sudo, bash 3.2): QD1 4 KiB re-read of a small region (drive read cache) on drive2 (idx 1)
# and drive1 (idx 0): idle spread, region-size sweep, outer vs middle, CPU-load control, and the
# recovery after the final run's K write sequence (perf.sh lines 78-87). Read flags = K-rd4K-qd1-* rows.
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../../.." && pwd)
OUT=${OUT:-$REPO/bench/out/krecheck}
mkdir -p $OUT; rm -f $OUT/STOP $OUT/DONE
[ -s $OUT/disks.ref ] || cp $REPO/bench/out/final/disks.ref $OUT/disks.ref
. "$SCRIPT_DIR/lib_final.sh"
SC=/opt/homebrew/bin/smartctl; TS=$OUT/ts.csv
echo "KRECHECK START $(date '+%F %T')" >> $LOG
setup || exit 1
ctx() { echo "$(date +%T) ctx $1: load $(sysctl -n vm.loadavg) top: $(ps -Ao pcpu,comm -r | sed -n 2,4p | awk '{printf "%s %s; ",$1,$2}')" >> $LOG
        for i in 0 1; do echo "$(date +%T),$1,${D[$i]},${SER[$i]},$($SC -A /dev/${D[$i]} 2>/dev/null | awk '$1==194 {print $10; exit}')" >> $OUT/temps.csv; done; }
Z16=3-3.000001; Z1M=3-3.0000058
SIZES="16KiB:3-3.000000114232 64KiB:3-3.000000387295 256KiB:3-3.000001479549 1MiB:3-3.000005848565 4MiB:3-3.000023324630 16MiB:3-3.000093228889 mid-16KiBrow:49-49.000001 mid-1MiBrow:49-49.0000058"
for z in $Z16 $Z1M $(echo $SIZES | tr ' ' '\n' | cut -d: -f2); do $SB -n -b 4K -q 1 -t 1 /dev/r${D[1]},r,$z,X | grep job >> $OUT/zones.txt; done
rd() { run "$1" -b 4K -q 1 -t $4 -T $TS -i 1 $(jobs r $3 $2); }
$CTL stats > $OUT/stats-start.txt 2>&1
ctx start

## 1. idle baseline: drives idle 5 min first
iostat -d -w 60 -c 6 ${D[0]} ${D[1]} > $OUT/idle-iostat.txt 2>&1
ctx idle
for i in 1 0; do
  s=${SER[$i]}
  for k in 1 2 3 4 5; do
    rd KR-idle-16KiB-region-r$k-$s $i $Z16 15
    rd KR-idle-1MiB-region-r$k-$s  $i $Z1M 15
  done
  for e in $SIZES; do rd KR-size-${e%%:*}-$s $i ${e#*:} 15; done
  if [ $i = 1 ]; then  # host-side control: one core busy (shallower CPU idle states)
    yes > /dev/null & Y=$!
    rd KR-cpubusy-16KiB-region-$s $i $Z16 15
    rd KR-cpubusy-1MiB-region-$s  $i $Z1M 15
    kill $Y; wait $Y 2>/dev/null
  fi
done
ctx idle-end

## 2. write burst = the final run's K write sequence, then the K re-reads at t = 0, 30, 60, 120, 300 s
burst() { local i=$1 p=$2
  run $p-wr4K-seq-qd1-nosync     -b 4K  -q 1 -t 15 $(jobs w 3-3.1 $i)
  run $p-wr4K-seq-qd1-dkiocsync  -b 4K  -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
  run $p-wr4K-seq-qd1-rawfullfsync -b 4K -q 1 -t 5 -F 1 $(jobs w 3-3.1 $i)
  run $p-wr64K-seq-qd1-nosync    -b 64K -q 1 -t 15 $(jobs w 3-3.1 $i)
  run $p-wr64K-seq-qd1-dkiocsync -b 64K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
  run $p-wr1M-seq-qd1-nosync     -b 1M  -q 1 -t 15 $(jobs w 3-3.1 $i)
  run $p-wr1M-seq-qd1-dkiocsync  -b 1M  -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
  run $p-wr4K-rand-qd1-nosync    -b 4K  -q 1 -t 15 -p rand $(jobs w 2-50 $i)
  run $p-wr4K-rand-qd1-dkiocsync -b 4K  -q 1 -t 15 -p rand -F 2 $(jobs w 2-50 $i)
  run $p-wr4K-seq-qd1-dkiocsync-all7 -b 4K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $ALL)
  local t0=$(date +%s) o now
  for o in 0 30 60 120 300; do
    now=$(date +%s); [ $((t0 + o)) -gt $now ] && sleep $((t0 + o - now))
    echo "$(date +%T) series $p t=$o actual $(( $(date +%s) - t0 ))" >> $LOG
    rd $p-t$o-16KiB-region-${SER[$i]} $i $Z16 10
    rd $p-t$o-1MiB-region-${SER[$i]}  $i $Z1M 10
  done
  ctx $p-end; }
burst 1 KRB1
burst 1 KRB2
burst 0 KRB3
umount7; $CTL stats > $OUT/stats-end.txt 2>&1
echo "KRECHECK END $(date '+%F %T')" >> $LOG
date '+%F %T' > $OUT/DONE
