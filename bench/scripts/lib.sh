# sourced by phase scripts (run as root)
OUT=~/sasbench/out; SB=~/sasbench/sasbench; R=$OUT/results.csv; LOG=$OUT/run.log
[ -e $OUT/STOP ] && { echo "STOP present, phase skipped" >> $LOG; exit 9; }
bash ~/sasbench/setup.sh > $OUT/setup.last 2>&1 || { echo "setup refused: $(tail -1 $OUT/setup.last)" >> $LOG; exit 1; }
D=($(awk '{print $1}' $OUT/disks.txt)); SER=($(awk '{print $2}' $OUT/disks.txt))
NDISK=${#D[@]}
[ "$NDISK" = 7 ] || { echo "refuse: $NDISK disks" >> $LOG; exit 1; }
for d in "${D[@]}"; do case $d in disk0|disk1|disk2|disk3) echo "refuse system disk $d" >> $LOG; exit 1;; esac; done
# jobs MODE ZONE idx... -> "/dev/rdiskN,MODE,ZONE,SERIAL" ...
jobs() { local m=$1 z=$2; shift 2; for i in "$@"; do printf '/dev/r%s,%s,%s,%s ' "${D[$i]}" "$m" "$z" "${SER[$i]}"; done; }
ALL="0 1 2 3 4 5 6"
run() { local label=$1; shift
  [ -e $OUT/STOP ] && { echo "STOP file present, skip $label" >> $LOG; exit 9; }
  echo "== $label $(date +%T) :: $*" >> $LOG
  $SB -o $R -l "$label" "$@" >> $LOG 2>&1; local rc=$?
  echo "rc=$rc $(date +%T)" >> $LOG
  if [ $rc = 3 ]; then echo "$(date +%T) DATA MISMATCH in $label" >> $OUT/ALERTS.txt; touch $OUT/STOP; exit 3; fi
  if grep -q "killed sasbench" $OUT/ALERTS.txt 2>/dev/null && [ $rc != 0 ]; then echo "aborted by watchdog in $label" >> $LOG; touch $OUT/STOP; exit 8; fi
  return $rc; }
warm() { $SB -t 3 -b 64K -q 1 -o $OUT/warm.csv -l "warm-$1" $(jobs r 10-11 $ALL) > /dev/null 2>&1; }
