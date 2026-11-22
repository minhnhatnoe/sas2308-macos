#!/bin/bash
# Thermal soak (run with sudo): 2 min idle baseline, 30 min all 7 drives 1 MiB sequential 50/50 R/W over the
# full LBA range at QD8 per drive, 5 min cool-down. Sample every 30 s -> soak.csv.
. "${FINAL_SCRIPTS:-$(dirname "$0")}/lib_final.sh"
SC=/opt/homebrew/bin/smartctl; C=$OUT/soak.csv; LOADS=${1:-1800}
echo "SOAK START $(date '+%F %T')" >> $LOG
setup || exit 1
echo "time,phase,ioc_C,$(echo ${SER[@]} | tr ' ' ','),tdie_max,tdie_avg,soc_all_max,pressure,drv_MBps,drv_IOPS,lat_avg_ms_interval,lat_max_ms_cum,io_timeouts,family_timeouts,reinits,tm_os,err_scsi,err_xport" > $C
prevn=; prevb=; prevl=
sample() { local ph=$1 s t dt i n b la tc
  $CTL stats > $OUT/.soakstats 2>/dev/null
  g() { awk -v k="$1" '{l=$0; sub(/^  /,"",l)} index(l,k)==1 && substr(l,length(k)+1,1)==" " {print $NF; exit}' $OUT/.soakstats; }
  n=$(g "IOs completed"); b=$(( $(g "Read bytes") + $(g "Write bytes") )); la=$(g "Latency avg us")
  local mbps= iops= lavg=
  if [ -n "$prevn" ]; then dt=$(( $(date +%s) - prevt )); [ $dt -lt 1 ] && dt=1
    mbps=$(( (b - prevb) / dt / 1000000 )); iops=$(( (n - prevn) / dt ))
    [ $((n - prevn)) -gt 0 ] && lavg=$(awk -v n1=$prevn -v a1=$prevl -v n2=$n -v a2=$la 'BEGIN{printf "%.1f", (n2*a2-n1*a1)/(n2-n1)/1000}')
  fi
  prevn=$n; prevb=$b; prevl=$la; prevt=$(date +%s)
  local dtemps="" hot=0
  for i in 0 1 2 3 4 5 6; do tc=$($SC -A /dev/${D[$i]} 2>/dev/null | awk '$1==194 {print $10; exit}'); dtemps="$dtemps,${tc:-NA}"; [ -n "$tc" ] && [ "$tc" -ge 60 ] && hot=1; done
  local ioc=$(g "IOC temperature (C)") pr=$(powermetrics -n 1 -i 200 --samplers thermal 2>/dev/null | awk -F': ' '/pressure level/ {print $2; exit}')
  echo "$(date +%T),$ph,$ioc$dtemps,$($SOC -s | awk '{print $1","$2","$4}'),$pr,$mbps,$iops,$lavg,$(( $(g "Latency max us") / 1000 )),$(g "IO timeouts"),$(g "Family timeouts"),$(g "Reinits"),$(g "TM requests from OS"),$(g "Err SCSI status"),$(g "Err transport")" >> $C
  if [ "${ioc:-0}" -ge 90 ] || [ $hot = 1 ]; then echo "$(date +%T) SOAK ABORT temp ioc=$ioc drives$dtemps" >> $OUT/ALERTS.txt; pkill -TERM -x sasbench; touch $OUT/STOP; fi
}
for k in 1 2 3 4 5; do sample idle; [ $k -lt 5 ] && sleep 30; done
touch $OUT/abort_on_alert; umount7
if ! stopped; then
  echo "== SOAK load ${LOADS}s $(date +%T)" >> $LOG
  $SB -o $R -l S-soak-seq-1M-mix50-q8-full -b 1M -q 8 -M 50 -t $LOADS -T $OUT/soak-int.csv -i 30 $(jobs m full $ALL) > $OUT/soak-sasbench.txt 2>&1 &
  P=$!; sleep 30
  while kill -0 $P 2>/dev/null; do sample load; sleep 29; done
  wait $P; echo "soak sasbench rc=$? $(date +%T)" >> $LOG
fi
rm -f $OUT/abort_on_alert
for k in 1 2 3 4 5 6 7 8 9 10 11; do sample cool; [ $k -lt 11 ] && sleep 30; done
$CTL stats > $OUT/stats-end.txt 2>&1
echo "SOAK DONE $(date '+%F %T')" >> $LOG
