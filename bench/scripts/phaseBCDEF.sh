# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
. "$(dirname "$0")/lib.sh"
warm B
# B: scaling, outer, 1M seq q2
for set in "1" "1 2" "1 2 3 4" "$ALL"; do n=$(echo $set | wc -w | tr -d ' ')
  run "B-read-${n}drv"  -b 1M -q 2 -t 30 $(jobs r outer $set)
done
for set in "1" "1 2" "1 2 3 4" "$ALL"; do n=$(echo $set | wc -w | tr -d ' ')
  run "B-write-${n}drv" -b 1M -q 2 -t 30 $(jobs w outer $set)
done
# C: write-all then read-all, outer and inner
run C-write-all-outer -b 1M -q 2 -t 30 $(jobs w outer $ALL)
run C-read-all-outer  -b 1M -q 2 -t 30 $(jobs r outer $ALL)
run C-write-all-inner -b 1M -q 2 -t 30 $(jobs w inner $ALL)
run C-read-all-inner  -b 1M -q 2 -t 30 $(jobs r inner $ALL)
# D: mixed
run D-4W3R-outer -b 1M -q 2 -t 30 $(jobs w outer 0 1 2 3) $(jobs r outer 4 5 6)
run D-3W4R-outer -b 1M -q 2 -t 30 $(jobs w outer 0 1 2) $(jobs r outer 3 4 5 6)
run D-mix70-seq-1M-q4  -b 1M  -q 4 -t 30 -M 70 $(jobs m 0-1.5 $ALL)
run D-mix70-rand-64K-q8 -b 64K -q 8 -t 30 -M 70 -p rand $(jobs m 2-50 $ALL)
# E: zone mixes
run E-write-4outer-3inner -b 1M -q 2 -t 30 $(jobs w outer 0 1 2 3) $(jobs w inner 4 5 6)
run E-read-4outer-3inner  -b 1M -q 2 -t 30 $(jobs r outer 0 1 2 3) $(jobs r inner 4 5 6)
run E-read-outer+inner-same-drive  -b 1M -q 2 -t 30 $(jobs r outer $ALL) $(jobs r inner $ALL)
run E-write-outer+inner-same-drive -b 1M -q 2 -t 30 $(jobs w outer $ALL) $(jobs w inner $ALL)
run E-readouter+writeinner-same-drive -b 1M -q 2 -t 30 $(jobs r outer $ALL) $(jobs w inner $ALL)
# F1: block-size sweep, seq read all 7
for bs in 4K 64K 128K 1M 4M; do run "F-seqread-$bs-q2" -b $bs -q 2 -t 20 $(jobs r outer $ALL); done
# F2: random, all 7, zone 2-50%
for bs in 4K 64K; do for qd in 1 8 32; do
  run "F-randread-$bs-qd$qd"  -b $bs -q $qd -t 20 -p rand $(jobs r 2-50 $ALL)
  run "F-randwrite-$bs-qd$qd" -b $bs -q $qd -t 20 -p rand $(jobs w 2-50 $ALL)
done; done
# F3: queue-limit probe, one drive alone
for qd in 1 8 32; do run "F-randread-4K-qd$qd-1drv" -b 4K -q $qd -t 20 -p rand $(jobs r 2-50 1); done
# F4: full-stroke random read
for qd in 1 32; do run "F-randread-4K-qd$qd-fullstroke" -b 4K -q $qd -t 20 -p rand $(jobs r full $ALL); done
echo "PHASE BCDEF DONE $(date +%T)" >> $LOG
