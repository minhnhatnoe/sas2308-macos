# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
. "$(dirname "$0")/lib_prof.sh"
setup || exit 1
$SB -o $R -l F-seqread-4K-q2-t3 -b 4K -q 2 -t 40 $(jobs r outer $ALL) > $OUT/sb-t3.txt 2>&1 & SBP=$!; sleep 8
i0=$(ios); dtrace -Z -q -s $PS/t3.d > $OUT/dt-t3.txt 2> $OUT/dt-t3.err; i1=$(ios); echo "win t3 ios=$((i1-i0))" >> $LOG
wait $SBP
