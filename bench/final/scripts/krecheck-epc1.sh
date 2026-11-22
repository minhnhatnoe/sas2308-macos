#!/bin/bash
# EPC read-only probe (run with sudo). Args: the serial numbers of the drives to probe.
# OUT = output directory (default bench/out/krecheck/epc)
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../../.." && pwd)
[ $# -gt 0 ] || { echo "usage: $0 SERIAL..." >&2; exit 2; }
OUT=${OUT:-$REPO/bench/out/krecheck/epc}; mkdir -p $OUT
SC=/opt/homebrew/bin/smartctl
bash "$REPO/bench/scripts/sasdisks.sh" > $OUT/disks.txt 2>/dev/null
cat $OUT/disks.txt
for s in "$@"; do
  d=$(awk -v s=$s '$2==s {print $1}' $OUT/disks.txt)
  case $d in disk[4-9]|disk1[0-9]) ;; *) echo "bad disk for $s: '$d'"; continue;; esac
  $SC -g epc /dev/$d > $OUT/g-epc-$s.txt 2>&1; echo "$s $d -g epc rc=$?"
  $SC -d sat -g epc /dev/$d > $OUT/g-epc-sat-$s.txt 2>&1; echo "$s -d sat -g epc rc=$?"
  $SC -x /dev/$d > $OUT/x-$s.txt 2>&1; echo "$s -x rc=$?"
  $SC -i /dev/$d > $OUT/i-$s.txt 2>&1
  $SC -l devstat /dev/$d > $OUT/devstat-$s.txt 2>&1
  $SC -g all /dev/$d > $OUT/g-all-$s.txt 2>&1
done
$SC -h > $OUT/smartctl-h.txt 2>&1
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" $OUT
