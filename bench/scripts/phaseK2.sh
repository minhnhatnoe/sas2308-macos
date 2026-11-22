# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# K2: flush tests with ioctl DKIOCSYNCHRONIZECACHE (F_FULLFSYNC is refused on raw devices)
. "$(dirname "$0")/lib.sh"
i=1
run K2-wr4K-rand-qd1-nosync     -b 4K -q 1 -t 15 -p rand $(jobs w 2-50 $i)
run K2-wr4K-rand-qd1-dkiocsync  -b 4K -q 1 -t 15 -p rand -F 2 $(jobs w 2-50 $i)
run K2-wr64K-seq-qd1-dkiocsync  -b 64K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
run K2-wr1M-seq-qd1-nosync      -b 1M -q 1 -t 15 $(jobs w 3-3.1 $i)
run K2-wr1M-seq-qd1-dkiocsync   -b 1M -q 1 -t 15 -F 2 $(jobs w 3-3.1 $i)
run K2-wr4K-seq-qd1-dkiocsync-all7 -b 4K -q 1 -t 15 -F 2 $(jobs w 3-3.1 $ALL)
OLD=$($CTL show | awk '/Debug mask/ {print $NF; exit}')
/usr/bin/log stream --level debug --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"' > $OUT/trace-sync2.txt 2>&1 &
LP=$!; sleep 2
$CTL mask +trace >> $LOG 2>&1
run K2-trace-dkiocsync -b 4K -q 1 -t 2 -F 2 $(jobs w 3-3.1 $i)
$CTL mask $OLD >> $LOG 2>&1
sleep 2; kill $LP
echo "mask restored to $($CTL show | awk '/Debug mask/ {print $NF; exit}') (was $OLD)" >> $LOG
echo "PHASE K2 DONE $(date +%T)" >> $LOG
