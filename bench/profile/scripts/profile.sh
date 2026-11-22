# step 2: one long F1 4 KiB load (7 drives, QD2, outer); dtrace windows of 20 s, idle gaps between,
# IOs counted around each window. Args: LO HI (our kext text range for prof.d, hex).
. "$(dirname "$0")/lib_prof.sh"
LO=${1:-0}; HI=${2:-0}; P=$PS
setup || exit 1
$SB -o $R -l F-seqread-4K-q2-prof -b 4K -q 2 -t 200 -T $OUT/prof-int.csv -i 5 $(jobs r outer $ALL) > $OUT/sb-prof.txt 2>&1 &
SBP=$!; sleep 8
win() { local n=$1; shift; local i0=$(ios) t0=$(date +%s)
  dtrace -Z -q -s $P/$n.d "$@" > $OUT/dt-$n.txt 2> $OUT/dt-$n.err
  local i1=$(ios) t1=$(date +%s); echo "win $n ios=$((i1-i0)) secs=$((t1-t0))" >> $LOG; sleep 6; }
win prof $LO $HI; win sched; win t1; win t2; win locks
kill -TERM $SBP; wait $SBP; echo "sasbench rc=$? $(date +%T)" >> $LOG
echo PDONE >> $LOG
