# sourced by the profile scripts (run as root, bash 3.2). TAG = build tag (UUID prefix).
B=~/sasbench; SB=$B/sasbench; CTL=~/sas2308-dext/tools/sas2308ctl
OUT=$B/profile/${TAG:?}; mkdir -p $OUT; LOG=$OUT/run.log; R=$OUT/results.csv
# the 7 ST18000NM000J behind SAS2308HBA (sasdisks.sh), exactly 7, never disk0-3, unmounted
setup() {
  bash $B/sasdisks.sh > $OUT/disks.txt 2>/dev/null || { echo "NO DISKS" >> $LOG; return 1; }
  [ "$(wc -l < $OUT/disks.txt | tr -d ' ')" = 7 ] || { echo "REFUSE: not 7 drives" >> $LOG; return 1; }
  D=($(awk '{print $1}' $OUT/disks.txt)); SER=($(awk '{print $2}' $OUT/disks.txt))
  for d in "${D[@]}"; do case $d in disk0|disk1|disk2|disk3) echo "REFUSE $d" >> $LOG; return 1;; esac; done
  for d in "${D[@]}"; do diskutil unmountDisk /dev/$d > /dev/null 2>&1; done
  local m=$(mount | grep -cE "/dev/($(echo ${D[@]} | tr ' ' '|'))s")
  [ "$m" = 0 ] || { echo "REFUSE: mounted" >> $LOG; return 1; }
  echo "$(date +%T) setup ok ${D[*]} kext $(kmutil showloaded --list-only 2>/dev/null | grep org.sas2308 | awk '{print $7}')" >> $LOG
}
jobs() { local m=$1 z=$2; shift 2; for i in "$@"; do printf '/dev/r%s,%s,%s,%s ' "${D[$i]}" "$m" "$z" "${SER[$i]}"; done; }
ALL="0 1 2 3 4 5 6"
ios() { $CTL stats 2>/dev/null | awk '/IOs completed/{print $NF}'; }
