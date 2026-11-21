# read-only IDENTIFY dump on one still-SMART-capable drive of the same model/firmware (root)
OUT=~/sasbench/krecheck/epc; SC=/opt/homebrew/bin/smartctl
d=$(awk '$2=="drive3" {print $1}' $OUT/disks.txt); case $d in disk[4-9]|disk1[0-9]) ;; *) echo bad; exit 1;; esac
$SC -i --identify=wnb /dev/$d > $OUT/identify-drive3.txt 2>&1; echo "rc=$?"
chown "$SUDO_USER" $OUT/identify-drive3.txt
