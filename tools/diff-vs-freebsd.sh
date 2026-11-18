#!/bin/sh
# Compare the ported core with FreeBSD sys/dev/mps (ref/FREEBSD_REF.txt has the commit).
#
#   tools/diff-vs-freebsd.sh            diff -u of every ported file, then a summary
#   tools/diff-vs-freebsd.sh -s         summary only
#   tools/diff-vs-freebsd.sh mps_sas.c  one file
#
# Mechanical differences are stripped from both sides before diffing: #include
# lines and __FBSDID. Everything else (including /* XNU: ... */ deviations)
# shows up. REF overrides the FreeBSD tree (default ref/freebsd-src/sys/dev/mps).
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
ref=${REF:-$top/ref/freebsd-src/sys/dev/mps}
files="mpsvar.h mps_sas.h mps_table.h mps.c mps_config.c mps_sas.c mps_sas_lsi.c mps_table.c"
summary_only=0
if [ "${1:-}" = "-s" ]; then summary_only=1; shift; fi
[ $# -gt 0 ] && files="$*"
[ -d "$ref" ] || { echo "no FreeBSD tree at $ref (see ref/FREEBSD_REF.txt)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
strip() { grep -vE '^[[:space:]]*#[[:space:]]*include[[:space:]]|^__FBSDID' "$1" || true; }

printf '%-14s %7s %7s %7s %7s %7s %5s\n' file freebsd ours common added removed xnu > "$tmp/sum"
tf=0; to=0; tc=0; ta=0; tr=0; tx=0
for f in $files; do
	if [ ! -f "$top/core/$f" ]; then
		printf '%-14s %s\n' "$f" "(not ported yet)" >> "$tmp/sum"
		continue
	fi
	strip "$ref/$f" > "$tmp/a"
	strip "$top/core/$f" > "$tmp/b"
	if [ $summary_only -eq 0 ]; then
		diff -u --label "freebsd/$f" --label "core/$f" "$tmp/a" "$tmp/b" || true
	fi
	nf=$(wc -l < "$tmp/a"); no=$(wc -l < "$tmp/b")
	nc=$(diff --unchanged-line-format='x
' --old-line-format='' --new-line-format='' "$tmp/a" "$tmp/b" | wc -l)
	na=$((no - nc)); nr=$((nf - nc))
	nx=$(grep -c 'XNU' "$tmp/b" || true)
	printf '%-14s %7d %7d %7d %7d %7d %5d\n' "$f" "$nf" "$no" "$nc" "$na" "$nr" "$nx" >> "$tmp/sum"
	tf=$((tf + nf)); to=$((to + no)); tc=$((tc + nc)); ta=$((ta + na)); tr=$((tr + nr)); tx=$((tx + nx))
done
printf '%-14s %7d %7d %7d %7d %7d %5d\n' total "$tf" "$to" "$tc" "$ta" "$tr" "$tx" >> "$tmp/sum"
[ $summary_only -eq 0 ] && echo
echo "== vs FreeBSD $(sed -n 's/.*commit \([0-9a-f]\{12\}\).*/\1/p' "$top/ref/FREEBSD_REF.txt" 2>/dev/null | head -1)" \
     "(common = lines unchanged; added/removed = diff -u +/- lines; xnu = lines mentioning XNU)"
cat "$tmp/sum"
