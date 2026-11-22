# re-read the data written by an earlier G run (no writes): $1 = tag in out/seeds
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
bash "$(dirname "$0")/phaseK3.sh"
. "$(dirname "$0")/lib.sh"
read -r TAG SEED1 SEED2 < <(grep "^$1 " $OUT/seeds | tail -1)
G1Z="1.8-2 50.8-51 99.8-100"; G2Z="1.5-1.8 50.5-50.8 99.5-99.8"
j1=""; for z in $G1Z; do j1="$j1 $(jobs c $z $ALL)"; done
j2=""; for z in $G2Z; do j2="$j2 $(jobs c $z $ALL)"; done
run "G-check-$TAG-1M"  -b 1M  -q 4 -B 2G   -s $SEED1 $j1
run "G-check-$TAG-64K" -b 64K -q 8 -B 256M -p rand -s $SEED2 $j2
echo "PHASE Gcheck $TAG DONE $(date +%T)" >> $LOG
