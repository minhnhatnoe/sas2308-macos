# step 1: the F1 4 KiB point (sasbench -b 4K -q 2, 7 drives, outer), 3 x 30 s, plus 1 drive QD2 and QD1
. ~/sasbench/profile/lib_prof.sh
setup || exit 1
one() { local label=$1; shift
  $CTL stats > $OUT/stats-$label-pre.txt 2>&1; local t0=$(date +%s)
  $SB -o $R -l $label "$@" > $OUT/sb-$label.txt 2>&1; echo "$label rc=$? $(date +%T)" >> $LOG
  local t1=$(date +%s); $CTL stats > $OUT/stats-$label-post.txt 2>&1; echo "$label wall=$((t1-t0))" >> $LOG; }
for i in 1 2 3; do one F-seqread-4K-q2-r$i -b 4K -q 2 -t 30 $(jobs r outer $ALL); sleep 3; done
one F-seqread-4K-q2-1drv -b 4K -q 2 -t 30 $(jobs r outer 0); sleep 3
one F-seqread-4K-q1-1drv -b 4K -q 1 -t 30 $(jobs r outer 0)
one F-seqread-4K-q1-7drv -b 4K -q 1 -t 30 $(jobs r outer $ALL)
echo DONE >> $LOG
