#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# Background sampler (run with sudo; bash 3.2 compatible): driver stats every 15 s, drive temps
# every 60 s. Watchdog: IOC temp >= 90, non-operational state, or any increase of a watched
# error counter -> ALERT line; kills sasbench if out/abort_on_alert exists.
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../.." && pwd)
OUT=${1:-$REPO/bench/out/run}
CTL=${SAS2308CTL:-$REPO/tools/sas2308ctl}
SC=${SMARTCTL:-$(command -v smartctl || echo smartctl)}
mkdir -p "$OUT"
F="$OUT/monitor.csv"; T="$OUT/drivetemps.csv"; B="$OUT/.monbase"
KEYS="IOC temperature (C)|IOs completed|Read bytes|Write bytes|Latency avg us|Latency max us|Held now|Held total|Tasks rejected|IO timeouts|Family timeouts|Recoveries|Reinits|IOC faults|Diag resets|Err check condition|Err SCSI status|Err terminated|Err transport|Err device gone|TM requests from OS|Stale replies|Bad replies|Chain free low|Chain alloc failures|State"
WATCH="IO timeouts|Family timeouts|Recoveries|Reinits|IOC faults|Diag resets|Err transport|Err terminated|Err SCSI status|Err device gone|Stale replies|Bad replies|TM requests from OS|Chain alloc failures"
[ -s "$F" ] || echo "time,$KEYS" | tr '|' ',' > "$F"
[ -s "$T" ] || echo "time,disk,serial,temp_C" > "$T"
rm -f "$B"
n=0
while :; do
  ts=$(date +%H:%M:%S)
  $CTL stats 2>/dev/null > "$OUT/.stats"
  awk -v keys="$KEYS" -v watch="$WATCH" -v ts="$ts" -v csv="$F" -v base="$B" -v alerts="$OUT/ALERTS.txt" '
    BEGIN { nk=split(keys,K,"|"); nw=split(watch,W,"|");
            while ((getline l < base) > 0) { split(l,p,"\t"); old[p[1]]=p[2]; have=1 } }
    { l=$0; sub(/^  /,"",l); for (i=1;i<=nk;i++) if (index(l,K[i])==1) { r=substr(l,length(K[i])+1); gsub(/^ +| +$/,"",r); v[K[i]]=r } }
    END { line=ts; for (i=1;i<=nk;i++) line=line "," v[K[i]]; print line >> csv;
          a=""; t=v["IOC temperature (C)"]+0; if (t>=90) a=a " IOC_temp=" t;
          if (v["State"]!="operational") a=a " state=" v["State"];
          for (i=1;i<=nw;i++) { k=W[i]; if (have && v[k]+0 > old[k]+0) a=a " [" k " " old[k] "->" v[k] "]" }
          printf "" > base; for (i=1;i<=nw;i++) print W[i] "\t" v[W[i]] >> base;
          if (a!="") { print ts " ALERT" a >> alerts; exit 1 } }' "$OUT/.stats"
  if [ $? -ne 0 ] && [ -e "$OUT/abort_on_alert" ]; then pkill -TERM -x sasbench; echo "$ts killed sasbench" >> "$OUT/ALERTS.txt"; fi
  if [ $((n % 4)) -eq 0 ]; then
    while read -r d ser tgt; do
      tc=$($SC -A /dev/$d 2>/dev/null | awk '$1==194 {print $10; exit}')
      echo "$ts,$d,$ser,$tc" >> "$T"
    done < "$OUT/disks.txt"
  fi
  n=$((n+1))
  sleep 15
done
