#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# quick check of lib_final.sh and soak.sh (run with sudo); output in bench/out/smoke
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../../.." && pwd)
export OUT=$REPO/bench/out/smoke; rm -rf $OUT; mkdir -p $OUT
. "$SCRIPT_DIR/lib_final.sh"
setup || { echo setup failed; cat $LOG; exit 1; }
SNAP=1 run smoke-read -b 1M -q 2 -t 3 $(jobs r outer $ALL); echo "run rc=$?"
grep ALL $R | cut -d, -f1,11-17
cp "$SCRIPT_DIR/soak.sh" $OUT/soak-smoke.sh
FINAL_SCRIPTS=$SCRIPT_DIR bash $OUT/soak-smoke.sh 0 > $OUT/soak.err 2>&1 &
sleep 100; cat $OUT/soak.csv; cat $OUT/soak.err | head -5; kill %1 2>/dev/null
pkill -f soak-smoke.sh; ls $OUT $OUT/stats; tail -5 $LOG
