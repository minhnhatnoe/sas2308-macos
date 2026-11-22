# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# read-only IDENTIFY dump on one still-SMART-capable drive of the same model/firmware (run with sudo)
# Arg: its serial number (looked up in disks.txt of krecheck-epc1.sh). OUT as there.
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../../.." && pwd)
S=${1:?usage: $0 SERIAL}; OUT=${OUT:-$REPO/bench/out/krecheck/epc}; SC=${SMARTCTL:-$(command -v smartctl || echo smartctl)}
d=$(awk -v s="$S" '$2==s {print $1}' $OUT/disks.txt); case $d in disk[4-9]|disk1[0-9]) ;; *) echo bad; exit 1;; esac
$SC -i --identify=wnb /dev/$d > $OUT/identify-$S.txt 2>&1; echo "rc=$?"
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" $OUT/identify-$S.txt
