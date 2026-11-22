# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# H: soak, $1 minutes
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
bash "$(dirname "$0")/phaseK2.sh"
. "$(dirname "$0")/lib.sh"
SECS=$(( ${1:-60} * 60 ))
warm H
echo "== H soak ${1:-60} min $(date +%T)" >> $LOG
$SB -o $R -l H-soak-seq-1M -b 1M -q 2 -t $SECS -T $OUT/soak-seq.csv -i 30 $(jobs w 0-1.5 0 1 2 3) $(jobs r 98-99.5 4 5 6) > $OUT/soak-seq.txt 2>&1 &
P1=$!
$SB -o $R -l H-soak-rand-64K-mix70 -b 64K -q 8 -M 70 -p rand -t $SECS -T $OUT/soak-rand.csv -i 30 $(jobs m 2-50 $ALL) > $OUT/soak-rand.txt 2>&1 &
P2=$!
wait $P1; r1=$?; wait $P2; r2=$?
echo "soak rc seq=$r1 rand=$r2 $(date +%T)" >> $LOG
echo "PHASE H DONE $(date +%T)" >> $LOG
