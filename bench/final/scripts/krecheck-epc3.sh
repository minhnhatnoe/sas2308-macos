# CHECK POWER MODE probe on disk6 (serial $1) + 3 s QD1 read sanity on disk5 (serial $2) (run with sudo)
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$SCRIPT_DIR/../../.." && pwd)
S6=${1:?usage: $0 SERIAL_OF_DISK6 SERIAL_OF_DISK5}; S5=${2:?usage: $0 SERIAL_OF_DISK6 SERIAL_OF_DISK5}
OUT=${OUT:-$REPO/bench/out/krecheck/epc}; SC=${SMARTCTL:-$(command -v smartctl || echo smartctl)}; SB=${SASBENCH:-$REPO/tools/sasbench}
d6=$(awk -v s="$S6" '$2==s {print $1}' $OUT/disks.txt); d5=$(awk -v s="$S5" '$2==s {print $1}' $OUT/disks.txt)
case "$d6$d5" in disk6disk5) ;; *) echo "mapping changed $d6 $d5"; exit 1;; esac
$SC -n standby,0 -i /dev/$d6 > $OUT/checkpower-$S6.txt 2>&1; echo "checkpower rc=$?"
diskutil unmountDisk /dev/$d5 >/dev/null 2>&1
$SB -b 4K -q 1 -t 3 /dev/r$d5,r,3-3.000001,$S5 > $OUT/sanity-read-$S5.txt 2>&1; echo "read rc=$?"
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" $OUT
