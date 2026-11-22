#!/bin/bash
# post-run rechecks (run with sudo): read-cache-hit QD1 on two drives, and the SYNCHRONIZE CACHE trace capture
. "$(dirname "$0")/lib_final.sh"
setup || exit 1
for i in 1 0; do
  run "R-rd4K-qd1-16KiB-region-${SER[$i]}" -b 4K -q 1 -t 10 $(jobs r 3-3.000001 $i)
  run "R-rd4K-qd1-1MiB-region-${SER[$i]}"  -b 4K -q 1 -t 10 $(jobs r 3-3.0000058 $i)
done
run R-wr4K-seq-qd1-dkiocsync -b 4K -q 1 -t 5 -F 2 $(jobs w 3-3.1 1)
OLDM=$($CTL show | awk '/SAS2308DebugMask/ {print $NF; exit}')
/usr/bin/log config --subsystem org.sas2308.driver.SAS2308HBA --mode "level:debug,persist:debug" >> $LOG 2>&1
sleep 2; T0=$(date '+%Y-%m-%d %H:%M:%S'); $CTL mask +trace >> $LOG 2>&1
run R-trace-dkiocsync -b 4K -q 1 -t 2 -F 2 $(jobs w 3-3.1 1)
$CTL mask $OLDM >> $LOG 2>&1; sleep 3
/usr/bin/log show --debug --style compact --start "$T0" --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"' 2>/dev/null | grep -E "IO (start|done)" > $OUT/trace-sync2.txt
/usr/bin/log config --subsystem org.sas2308.driver.SAS2308HBA --mode "level:info,persist:default" >> $LOG 2>&1
echo "recheck: mask $($CTL show | awk '/SAS2308DebugMask/ {print $NF; exit}') (was $OLDM); $(/usr/bin/log config --status --subsystem org.sas2308.driver.SAS2308HBA | cut -c1-80); trace lines $(wc -l < $OUT/trace-sync2.txt)" >> $LOG
umount7; $CTL stats > $OUT/stats-final.txt 2>&1
