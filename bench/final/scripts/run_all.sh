#!/bin/bash
# runner (root): stats before + reset, monitor, perf suite, thermal soak, monitor stop
B=~/sasbench; OUT=$B/final; CTL=~/sas2308-dext/tools/sas2308ctl
mkdir -p $OUT; rm -f $OUT/STOP $OUT/MONSTOP $OUT/DONE $OUT/abort_on_alert
date '+%F %T %Z' > $OUT/start.txt
$CTL show > $OUT/show-before.txt 2>&1; $CTL stats > $OUT/stats-before-reset.txt 2>&1
$CTL reset-stats >> $OUT/start.txt 2>&1
bash $B/sasdisks.sh > $OUT/disks.txt
bash $B/monitor_final.sh $OUT > $OUT/monitor.err 2>&1 &
MON=$!
bash $B/perf.sh > $OUT/perf.err 2>&1
bash $B/soak.sh ${SOAKSECS:-1800} > $OUT/soak.err 2>&1
touch $OUT/MONSTOP; sleep 16; kill $MON 2>/dev/null
$CTL show > $OUT/show-after.txt 2>&1
date '+%F %T %Z' > $OUT/DONE
