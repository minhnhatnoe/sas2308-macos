#!/bin/bash
# Re-derive the drive list, refuse unless 7, unmount them. Writes out/disks.txt.
OUT=~/sasbench/out; mkdir -p $OUT
bash ~/sasbench/sasdisks.sh > $OUT/disks.new || { echo "NO DISKS"; exit 1; }
n=$(wc -l < $OUT/disks.new | tr -d ' ')
cat $OUT/disks.new
if [ "$n" != 7 ]; then echo "REFUSE: $n drives qualify"; exit 1; fi
mv $OUT/disks.new $OUT/disks.txt
while read -r d s t; do diskutil unmountDisk /dev/$d >/dev/null 2>&1; done < $OUT/disks.txt
m=$(mount | grep -cE "/dev/($(awk '{print $1}' $OUT/disks.txt | paste -sd'|' -))s")
echo "mounted partitions on the 7: $m"
