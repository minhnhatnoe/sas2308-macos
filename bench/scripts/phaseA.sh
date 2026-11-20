. ~/sasbench/lib.sh
warm A
for i in $ALL; do
  run "A-read-outer-${D[$i]}"  -b 1M -q 2 -t 20 $(jobs r outer $i)
  run "A-write-outer-${D[$i]}" -b 1M -q 2 -t 20 $(jobs w outer $i)
  run "A-read-inner-${D[$i]}"  -b 1M -q 2 -t 20 $(jobs r inner $i)
  run "A-write-inner-${D[$i]}" -b 1M -q 2 -t 20 $(jobs w inner $i)
done
echo "== dd cross-check ${D[1]} $(date +%T)" >> $LOG
dd if=/dev/r${D[1]} of=/dev/null bs=1m count=4096 >> $LOG 2>&1
dd if=/dev/r${D[1]} of=/dev/null bs=1m count=4096 skip=$((17166000)) >> $LOG 2>&1
echo "PHASE A DONE $(date +%T)" >> $LOG
