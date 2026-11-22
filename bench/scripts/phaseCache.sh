# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
. "$(dirname "$0")/lib.sh"
warm K
i=1
# write cache: QD1 4K sequential writes, no sync / F_FULLFSYNC / DKIOCSYNCHRONIZECACHE
run K-wr4K-seq-qd1-nosync    -b 4K -q 1 -t 15 $(jobs w 3-3.1 $i)
run K-wr4K-seq-qd1-fullfsync -b 4K -q 1 -t 15 -F 1 $(jobs w 3-3.1 $i)
run K-wr4K-seq-qd1-dkiocsync -b 4K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
run K-wr64K-seq-qd1-nosync    -b 64K -q 1 -t 15 $(jobs w 3-3.1 $i)
run K-wr64K-seq-qd1-fullfsync -b 64K -q 1 -t 15 -F 1 $(jobs w 3-3.1 $i)
run K-wr4K-rand-qd1-nosync    -b 4K -q 1 -t 15 -p rand $(jobs w 2-50 $i)
run K-wr4K-rand-qd1-fullfsync -b 4K -q 1 -t 15 -p rand -F 1 $(jobs w 2-50 $i)
run K-wr4K-seq-qd1-fullfsync-all7 -b 4K -q 1 -t 15 -F 1 $(jobs w 3-3.1 $ALL)
# read cache: same 16 KiB / 1 MiB region re-read at QD1 vs random 4K reads
run K-rd4K-qd1-16KiB-region -b 4K -q 1 -t 10 $(jobs r 3-3.000001 $i)
run K-rd4K-qd1-1MiB-region  -b 4K -q 1 -t 10 $(jobs r 3-3.0000058 $i)
run K-rd4K-qd1-rand-2-50    -b 4K -q 1 -t 10 -p rand $(jobs r 2-50 $i)
# X: HBA/tunnel ceiling with drive read-cache hits (same ~5 MiB re-read, served at SATA link rate)
for set in "1" "1 2" "1 2 3 4" "$ALL"; do n=$(echo $set | wc -w | tr -d " ")
  run "X-cachehit-read-1M-q4-${n}drv" -b 1M -q 4 -t 15 $(jobs r 3-3.00003 $set)
done
run X-cachehit-read-128K-q8-7drv -b 128K -q 8 -t 15 $(jobs r 3-3.00003 $ALL)
run X-cachehit-read-4K-q8-7drv   -b 4K -q 8 -t 15 $(jobs r 3-3.00003 $ALL)
# confirm SYNCHRONIZE CACHE reaches the driver: trace on for a few seconds
OLD=$($CTL show | awk '/Debug mask/ {print $NF; exit}')
/usr/bin/log stream --level debug --predicate 'subsystem == "io.github.minhnhatnoe.sas2308"' > $OUT/trace-sync.txt 2>&1 &
LP=$!; sleep 2
$CTL mask +trace >> $LOG 2>&1
run K-trace-fullfsync -b 4K -q 1 -t 3 -F 1 $(jobs w 3-3.1 $i)
run K-trace-nosync    -b 4K -q 1 -t 1 $(jobs w 3-3.1 $i)
$CTL mask $OLD >> $LOG 2>&1
sleep 2; kill $LP
echo "mask restored to $($CTL show | awk '/Debug mask/ {print $NF; exit}') (was $OLD)" >> $LOG
echo "PHASE CACHE DONE $(date +%T)" >> $LOG
