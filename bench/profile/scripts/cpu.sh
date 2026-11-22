. "$(dirname "$0")/lib_prof.sh"
setup || exit 1
sysctl hw.perflevel0.logicalcpu hw.perflevel1.logicalcpu hw.perflevel2.logicalcpu hw.perflevel0.name hw.perflevel1.name hw.perflevel2.name > $OUT/cpu-topo.txt 2>&1
$SB -o $R -l F-seqread-4K-q2-cpu -b 4K -q 2 -t 30 $(jobs r outer $ALL) > $OUT/sb-cpu.txt 2>&1 & SBP=$!; sleep 8
dtrace -Z -q -s $PS/cpu.d $1 > $OUT/dt-cpu.txt 2> $OUT/dt-cpu.err
wait $SBP
