#!/bin/bash
# runner (run with sudo): stats before + reset, monitor, perf suite, thermal soak, monitor stop
# OUT = output directory (default bench/out/final)
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../../.." && pwd)
export OUT=${OUT:-$REPO/bench/out/final}; CTL=${SAS2308CTL:-$REPO/tools/sas2308ctl}
mkdir -p $OUT; rm -f $OUT/STOP $OUT/MONSTOP $OUT/DONE $OUT/abort_on_alert
date '+%F %T %Z' > $OUT/start.txt
$CTL show > $OUT/show-before.txt 2>&1; $CTL stats > $OUT/stats-before-reset.txt 2>&1
$CTL reset-stats >> $OUT/start.txt 2>&1
bash "$REPO/bench/scripts/sasdisks.sh" > $OUT/disks.txt
bash "$SCRIPT_DIR/monitor_final.sh" $OUT > $OUT/monitor.err 2>&1 &
MON=$!
bash "$SCRIPT_DIR/perf.sh" > $OUT/perf.err 2>&1
bash "$SCRIPT_DIR/soak.sh" ${SOAKSECS:-1800} > $OUT/soak.err 2>&1
touch $OUT/MONSTOP; sleep 16; kill $MON 2>/dev/null
$CTL show > $OUT/show-after.txt 2>&1
date '+%F %T %Z' > $OUT/DONE
