#!/bin/bash
export OUT=~/sasbench/smoke; rm -rf $OUT; mkdir -p $OUT
. ~/sasbench/lib_final.sh
setup || { echo setup failed; cat $LOG; exit 1; }
SNAP=1 run smoke-read -b 1M -q 2 -t 3 $(jobs r outer $ALL); echo "run rc=$?"
grep ALL $R | cut -d, -f1,11-17
cp $B/soak.sh $OUT/soak-smoke.sh
bash $OUT/soak-smoke.sh 0 > $OUT/soak.err 2>&1 &
sleep 100; cat $OUT/soak.csv; cat $OUT/soak.err | head -5; kill %1 2>/dev/null
pkill -f soak-smoke.sh; ls $OUT $OUT/stats; tail -5 $LOG
