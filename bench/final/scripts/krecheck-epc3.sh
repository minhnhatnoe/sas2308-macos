# CHECK POWER MODE probe on disk6 (drive3) + 3 s QD1 read sanity on drive2 (root)
OUT=~/sasbench/krecheck/epc; SC=/opt/homebrew/bin/smartctl; SB=~/sasbench/sasbench
d6=$(awk '$2=="drive3" {print $1}' $OUT/disks.txt); d5=$(awk '$2=="drive2" {print $1}' $OUT/disks.txt)
case "$d6$d5" in disk6disk5) ;; *) echo "mapping changed $d6 $d5"; exit 1;; esac
$SC -n standby,0 -i /dev/$d6 > $OUT/checkpower-drive3.txt 2>&1; echo "checkpower rc=$?"
diskutil unmountDisk /dev/$d5 >/dev/null 2>&1
$SB -b 4K -q 1 -t 3 /dev/r$d5,r,3-3.000001,drive2 > $OUT/sanity-read-drive2.txt 2>&1; echo "read rc=$?"
chown "$SUDO_USER" $OUT/*
