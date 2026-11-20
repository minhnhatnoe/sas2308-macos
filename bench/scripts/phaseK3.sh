# K3: confirm SYNCHRONIZE CACHE reaches the driver (trace class for ~3 s, then mask back to 631)
. ~/sasbench/lib.sh
CTL=~/sas2308-dext/tools/sas2308ctl
/usr/bin/log stream --timeout 10s --level debug --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"' > $OUT/trace-sync3.txt 2>&1 &
LP=$!; sleep 2
$CTL mask 1655 >> $LOG 2>&1
run K3-trace-dkiocsync -b 4K -q 1 -t 2 -F 2 $(jobs w 3-3.1 1)
run K3-trace-nosync    -b 4K -q 1 -t 0.05 $(jobs w 3-3.1 1)
$CTL mask 631 >> $LOG 2>&1
wait $LP
echo "K3 mask now $($CTL show | awk '/SAS2308DebugMask/ {print $NF; exit}')" >> $LOG
echo "PHASE K3 DONE $(date +%T)" >> $LOG
