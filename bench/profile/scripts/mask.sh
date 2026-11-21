# runtime A/B of the log checks (batch E candidate): debug mask 35 (now) / 0 / 631 (2026-11-20 run), F1 4 KiB 20 s each, restored to 35
. ~/sasbench/profile/lib_prof.sh
setup || exit 1
trap '$CTL mask 35 > /dev/null 2>&1' EXIT
for m in 35 0 631 35 0 631; do
  $CTL mask $m > /dev/null 2>&1; echo "mask $m -> $($CTL show | awk '/DebugMask/{print $2}')" >> $LOG
  $SB -o $R -l F-seqread-4K-q2-mask$m -b 4K -q 2 -t 20 $(jobs r outer $ALL) > /dev/null 2>&1; sleep 3
done
$CTL mask 35 > /dev/null 2>&1; echo "mask restored $($CTL show | awk '/DebugMask/{print $2}')" >> $LOG
