# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# sourced by the final-run phase scripts (bash 3.2): run them with sudo.
# OUT = output directory (default bench/out/final); SASBENCH, SAS2308CTL, SOCSENS, FSBENCH override the tool paths.
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
FS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); REPO=$(cd "$FS/../../.." && pwd)
OUT=${OUT:-$REPO/bench/out/final}; SB=${SASBENCH:-$REPO/tools/sasbench}; CTL=${SAS2308CTL:-$REPO/tools/sas2308ctl}
SOC=${SOCSENS:-$FS/socsens}; FSB=${FSBENCH:-$FS/fsbench}; SASDISKS=$REPO/bench/scripts/sasdisks.sh
R=$OUT/results.csv; LOG=$OUT/run.log; mkdir -p $OUT
. "$REPO/bench/scripts/confirm.sh"   # setup() refuses unless SAS2308_YES_DESTROY names the drives
stopped() { [ -e $OUT/STOP ] || [ -s $OUT/ALERTS.txt ]; }
# re-derive the 7 drives (IORegistry child of SAS2308HBA + diskutil Protocol SAS, model, size, External),
# refuse unless exactly 7, never disk0-3, same serial/target set as the first derivation; unmount them.
setup() {
  bash $SASDISKS > $OUT/disks.new 2>/dev/null || { echo "$(date +%T) NO DISKS" >> $LOG; touch $OUT/STOP; return 1; }
  local n=$(wc -l < $OUT/disks.new | tr -d ' ')
  [ "$n" = 7 ] || { echo "$(date +%T) REFUSE: $n drives qualify" >> $LOG; touch $OUT/STOP; return 1; }
  while read -r d s t; do case $d in disk0|disk1|disk2|disk3) echo "refuse system disk $d" >> $LOG; touch $OUT/STOP; return 1;; esac; done < $OUT/disks.new
  [ -s $OUT/disks.ref ] || cp $OUT/disks.new $OUT/disks.ref
  if ! cmp -s <(awk '{print $2,$3}' $OUT/disks.ref) <(awk '{print $2,$3}' $OUT/disks.new); then echo "$(date +%T) drive set changed" >> $LOG; cat $OUT/disks.new >> $LOG; touch $OUT/STOP; return 1; fi
  mv $OUT/disks.new $OUT/disks.txt
  confirm_destroy $OUT/disks.txt || { echo "$(date +%T) REFUSE: SAS2308_YES_DESTROY does not name the disks" >> $LOG; return 1; }
  D=($(awk '{print $1}' $OUT/disks.txt)); SER=($(awk '{print $2}' $OUT/disks.txt))
  umount7
  local m=$(mount | grep -cE "/dev/($(echo ${D[@]} | tr ' ' '|'))s")
  echo "$(date +%T) setup ok: ${D[*]} mounted=$m" >> $LOG
  [ "$m" = 0 ] || { echo "REFUSE: mounted" >> $LOG; touch $OUT/STOP; return 1; }
}
umount7() { local d; for d in "${D[@]}"; do diskutil unmountDisk /dev/$d > /dev/null 2>&1; done; }
jobs() { local m=$1 z=$2; shift 2; for i in "$@"; do printf '/dev/r%s,%s,%s,%s ' "${D[$i]}" "$m" "$z" "${SER[$i]}"; done; }
ALL="0 1 2 3 4 5 6"
# run LABEL sasbench-args...  (unmount + retry on "Resource busy"; SNAP=1 saves driver stats pre/post)
run() { local label=$1; shift
  if stopped; then echo "STOP/ALERT, skip $label" >> $LOG; return 9; fi
  [ -n "$SNAP" ] && $CTL stats > $OUT/stats/$label-pre.txt 2>&1
  echo "== $label $(date +%T) :: $*" >> $LOG
  local rc try
  for try in 1 2 3; do
    umount7
    $SB -o $R -l "$label" "$@" >> $LOG 2>&1; rc=$?
    if [ $rc = 1 ] && tail -3 $LOG | grep -q "Resource busy"; then echo "busy, retry $try" >> $LOG; sleep 5; continue; fi
    break
  done
  echo "rc=$rc $(date +%T)" >> $LOG
  [ -n "$SNAP" ] && $CTL stats > $OUT/stats/$label-post.txt 2>&1
  if [ $rc = 3 ]; then echo "$(date +%T) DATA MISMATCH in $label" >> $OUT/ALERTS.txt; touch $OUT/STOP; fi
  return $rc; }
warm() { $SB -t 3 -b 64K -q 1 -o $OUT/warm.csv -l "warm-$1" $(jobs r 10-11 $ALL) > /dev/null 2>&1; }
mkdir -p $OUT/stats
