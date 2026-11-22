#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# Re-derive the drive list, refuse unless 7, unmount them. Writes $OUT/disks.txt (run with sudo).
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../.." && pwd)
OUT=${OUT:-$REPO/bench/out/run}; mkdir -p $OUT
bash "$SCRIPT_DIR/sasdisks.sh" > $OUT/disks.new || { echo "NO DISKS"; exit 1; }
n=$(wc -l < $OUT/disks.new | tr -d ' ')
cat $OUT/disks.new
if [ "$n" != 7 ]; then echo "REFUSE: $n drives qualify"; exit 1; fi
mv $OUT/disks.new $OUT/disks.txt
while read -r d s t; do diskutil unmountDisk /dev/$d >/dev/null 2>&1; done < $OUT/disks.txt
m=$(mount | grep -cE "/dev/($(awk '{print $1}' $OUT/disks.txt | paste -sd'|' -))s")
echo "mounted partitions on the 7: $m"
