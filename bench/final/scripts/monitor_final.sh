#!/bin/bash
# Background sampler for the final run (root, bash 3.2): driver stats every 15 s -> monitor.csv;
# SMART 194 of each drive + SoC sensors every 60 s -> drivetemps.csv, soc.csv.
# Watchdog: IOC temp >= 90, drive >= 60, state != operational, or any increase of a watched counter
# -> ALERT line in ALERTS.txt, and sasbench is killed (SIGINT) if OUT/abort_on_alert exists.
OUT=${1:-~/sasbench/final}
CTL=~/sas2308-dext/tools/sas2308ctl; SC=/opt/homebrew/bin/smartctl; SOC=~/sasbench/socsens
F="$OUT/monitor.csv"; T="$OUT/drivetemps.csv"; S="$OUT/soc.csv"; Bf="$OUT/.monbase"
KEYS="IOC temperature (C)|IOs completed|Read bytes|Write bytes|Latency avg us|Latency max us|Held now|Held total|Tasks rejected|IO timeouts|Family timeouts|Recoveries|Reinits|IOC faults|Diag resets|Err check condition|Err SCSI status|Err terminated|Err transport|Err device gone|TM requests from OS|TM requests failed|TM timeouts|TM late replies|Held timeouts|Lost completion kicks|KASSERT hits|Controller dead|Stale replies|Chain free low|Chain alloc failures|Recovered sense (not errors)|State"
WATCH="IO timeouts|Family timeouts|Recoveries|Reinits|IOC faults|Diag resets|Err transport|Err terminated|Err device gone|Stale replies|TM requests from OS|TM requests failed|TM timeouts|Held timeouts|Lost completion kicks|KASSERT hits|Controller dead|Chain alloc failures"
[ -s "$F" ] || echo "time,$KEYS" | tr '|' ',' > "$F"
[ -s "$T" ] || echo "time,disk,serial,temp_C" > "$T"
[ -s "$S" ] || echo "time,tdie_max,tdie_avg,n_tdie,all_max" > "$S"
rm -f "$Bf"; n=0
alert() { echo "$1" >> "$OUT/ALERTS.txt"; if [ -e "$OUT/abort_on_alert" ]; then pkill -INT -x sasbench; echo "$(date +%T) killed sasbench" >> "$OUT/ALERTS.txt"; fi; }
while [ ! -e "$OUT/MONSTOP" ]; do
  ts=$(date +%H:%M:%S)
  $CTL stats 2>/dev/null > "$OUT/.stats"
  a=$(awk -v keys="$KEYS" -v watch="$WATCH" -v ts="$ts" -v csv="$F" -v base="$Bf" '
    BEGIN { nk=split(keys,K,"|"); nw=split(watch,W,"|");
            while ((getline l < base) > 0) { split(l,p,"\t"); old[p[1]]=p[2]; have=1 } }
    { l=$0; sub(/^  /,"",l); for (i=1;i<=nk;i++) if (index(l,K[i])==1 && substr(l,length(K[i])+1,1)==" ") { r=substr(l,length(K[i])+1); gsub(/^ +| +$/,"",r); v[K[i]]=r } }
    END { line=ts; for (i=1;i<=nk;i++) line=line "," v[K[i]]; print line >> csv;
          a=""; t=v["IOC temperature (C)"]+0; if (t>=90) a=a " IOC_temp=" t;
          if (v["State"]!="operational") a=a " state=" v["State"];
          for (i=1;i<=nw;i++) { k=W[i]; if (have && v[k]+0 > old[k]+0) a=a " [" k " " old[k] "->" v[k] "]" }
          printf "" > base; for (i=1;i<=nw;i++) print W[i] "\t" v[W[i]] >> base;
          print a }' "$OUT/.stats")
  [ -n "$a" ] && alert "$ts ALERT$a"
  if [ $((n % 4)) -eq 0 ]; then
    while read -r d ser tgt; do
      tc=$($SC -A /dev/$d 2>/dev/null | awk '$1==194 {print $10; exit}')
      echo "$ts,$d,$ser,$tc" >> "$T"
      [ -n "$tc" ] && [ "$tc" -ge 60 ] && alert "$ts ALERT drive $d $ser ${tc}C"
    done < "$OUT/disks.txt"
    echo "$ts,$($SOC -s | tr ' ' ',')" >> "$S"
  fi
  n=$((n+1))
  sleep 15
done
