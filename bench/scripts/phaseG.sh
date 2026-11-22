# G: integrity. $1 = tag (early/final). Seeds from out/seeds (written here).
. "$(dirname "$0")/lib.sh"
TAG=$1; SEED1=$(date +%s); SEED2=$((SEED1+1))
echo "$TAG $SEED1 $SEED2" >> $OUT/seeds
warm G
G1Z="1.8-2 50.8-51 99.8-100"; G2Z="1.5-1.8 50.5-50.8 99.5-99.8"
j1=""; for z in $G1Z; do j1="$j1 $(jobs v $z $ALL)"; done
j2=""; for z in $G2Z; do j2="$j2 $(jobs v $z $ALL)"; done
run "G-$TAG-1M-seq-q4"   -b 1M  -q 4 -B 2G   -s $SEED1 $j1
run "G-$TAG-64K-rand-q8" -b 64K -q 8 -B 256M -p rand -s $SEED2 $j2
echo "PHASE G $TAG DONE $(date +%T)" >> $LOG
