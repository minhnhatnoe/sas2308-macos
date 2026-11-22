#!/bin/bash
# q2048 queue-depth points (run with sudo). OUT = output directory (default bench/out/q2048)
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../.." && pwd)
OUT=${OUT:-$REPO/bench/out/q2048}; SB=${SASBENCH:-$REPO/tools/sasbench}; CTL=${SAS2308CTL:-$REPO/tools/sas2308ctl}
R=$OUT/results.csv; LOG=$OUT/run.log
mkdir -p $OUT; rm -f $OUT/STOP $OUT/DONE
echo "start $(date +%T)" >> $LOG
bash "$SCRIPT_DIR/sasdisks.sh" > $OUT/disks.new || { echo "NO DISKS" >> $LOG; touch $OUT/STOP $OUT/DONE; exit 1; }
n=$(wc -l < $OUT/disks.new | tr -d ' ')
[ "$n" = 7 ] || { echo "REFUSE: $n drives" >> $LOG; touch $OUT/STOP $OUT/DONE; exit 1; }
mv $OUT/disks.new $OUT/disks.txt; cat $OUT/disks.txt >> $LOG
. "$SCRIPT_DIR/confirm.sh"
confirm_destroy $OUT/disks.txt || { echo "REFUSE: SAS2308_YES_DESTROY does not name the disks" >> $LOG; touch $OUT/STOP $OUT/DONE; exit 1; }
while read -r d s t; do case $d in disk0|disk1|disk2|disk3) echo "refuse $d" >> $LOG; touch $OUT/STOP $OUT/DONE; exit 1;; esac; diskutil unmountDisk /dev/$d >/dev/null 2>&1; done < $OUT/disks.txt
m=$(mount | grep -cE "/dev/($(awk '{print $1}' $OUT/disks.txt | paste -sd'|' -))s"); echo "mounted on the 7: $m" >> $LOG
[ "$m" = 0 ] || { echo "REFUSE: mounted" >> $LOG; touch $OUT/STOP $OUT/DONE; exit 1; }
D=($(awk '{print $1}' $OUT/disks.txt)); SER=($(awk '{print $2}' $OUT/disks.txt))
jobs() { local m=$1 z=$2; shift 2; for i in "$@"; do printf '/dev/r%s,%s,%s,%s ' "${D[$i]}" "$m" "$z" "${SER[$i]}"; done; }
ALL="0 1 2 3 4 5 6"
touch $OUT/abort_on_alert
bash "$SCRIPT_DIR/q_monitor.sh" $OUT > /dev/null 2>&1 &
MON=$!
run() { local label=$1; shift
  if [ -e $OUT/STOP ] || [ -s $OUT/ALERTS.txt ]; then echo "STOP/ALERT, skip $label" >> $LOG; return 9; fi
  $CTL stats > $OUT/stats-$label-pre.txt 2>&1
  echo "== $label $(date +%T) :: $*" >> $LOG
  $SB -o $R -l "$label" "$@" >> $LOG 2>&1; local rc=$?
  echo "rc=$rc $(date +%T)" >> $LOG
  $CTL stats > $OUT/stats-$label-post.txt 2>&1
  t=$(awk '/IOC temperature \(C\)/ {print $NF}' $OUT/stats-$label-post.txt)
  echo "IOC temp after $label: $t" >> $LOG
  [ "$rc" = 3 ] && { echo "$(date +%T) DATA MISMATCH in $label" >> $OUT/ALERTS.txt; touch $OUT/STOP; }
  [ -n "$t" ] && [ "$t" -ge 90 ] && { echo "$(date +%T) IOC temp $t" >> $OUT/ALERTS.txt; touch $OUT/STOP; }
  [ -s $OUT/ALERTS.txt ] && touch $OUT/STOP
  return $rc; }
run Q-rr4K-qd32  -b 4K -q 32  -t 120 -p rand $(jobs r 2-50 $ALL)
run Q-rr4K-qd64  -b 4K -q 64  -t 120 -p rand $(jobs r 2-50 $ALL)
run Q-rr4K-qd128 -b 4K -q 128 -t 120 -p rand $(jobs r 2-50 $ALL)
run Q-rw4K-qd32  -b 4K -q 32  -t 120 -p rand $(jobs w 2-50 $ALL)
run Q-rw4K-qd64  -b 4K -q 64  -t 120 -p rand $(jobs w 2-50 $ALL)
run Q-seq1M-read-q2 -b 1M -q 2 -t 120 $(jobs r outer $ALL)
SEED=$(date +%s); echo "seeds $SEED $((SEED+1))" >> $LOG
run Q-verify-1M-seq-q4   -b 1M  -q 4  -B 1G   -s $SEED $(jobs v 1.8-2 $ALL)
run Q-verify-64K-rand-q64 -b 64K -q 64 -B 256M -p rand -s $((SEED+1)) $(jobs v 1.5-1.8 $ALL)
kill $MON 2>/dev/null; rm -f $OUT/abort_on_alert
$CTL stats > $OUT/stats-end.txt 2>&1
echo "end $(date +%T)" >> $LOG
touch $OUT/DONE
