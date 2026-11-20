#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Noe
#
# Compare the ported core with pristine FreeBSD sys/dev/mps.
#
#   tools/diff-vs-freebsd.sh            diff -u of every modified upstream file, then a summary
#   tools/diff-vs-freebsd.sh -s         summary only
#   tools/diff-vs-freebsd.sh mps_sas.c  one file
#
# Baseline: the vendor branch (git show vendor/freebsd-mps:core/<file>; VENDOR
# overrides the branch). REF=<freebsd-src>/sys/dev/mps diffs against a FreeBSD
# tree instead, and warns when that tree is not at the commit core/VENDOR.txt
# on the vendor branch names.
#
# This is a filtered view of `git diff vendor/freebsd-mps -- core/`: only the
# modified upstream files of the path map in core/VENDOR.txt (not core/mpi/,
# not our new files), with #include and __FBSDID lines stripped from both
# sides. Everything else (including /* XNU: ... */ deviations) shows up.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
vendor=${VENDOR:-vendor/freebsd-mps}
summary_only=0
if [ "${1:-}" = "-s" ]; then summary_only=1; shift; fi
git -C "$top" rev-parse -q --verify "$vendor^{commit}" >/dev/null ||
	{ echo "no branch $vendor (see core/VENDOR.txt)" >&2; exit 1; }
vcommit=$(git -C "$top" show "$vendor:core/VENDOR.txt" | sed -n 's/^Commit: *\([0-9a-f]\{40\}\).*/\1/p')
files=$(git -C "$top" show "$vendor:core/VENDOR.txt" |
	sed -n 's|^sys/dev/mps/\([^/ ]*\) *-> *core/\([^ ]*\).*|\2|p')
[ $# -gt 0 ] && files="$*"
if [ -n "${REF:-}" ]; then
	ref=$REF
	[ -d "$ref" ] || { echo "no FreeBSD tree at $ref" >&2; exit 1; }
	rcommit=$(git -C "$ref" rev-parse HEAD 2>/dev/null || echo unknown)
	[ "$rcommit" = "$vcommit" ] || echo "warning: $ref is at $rcommit, $vendor at $vcommit" >&2
	upstream() { cat "$ref/$1"; }
	label="FreeBSD tree $ref at $(echo "$rcommit" | cut -c1-12)"
else
	upstream() { git -C "$top" show "$vendor:core/$1"; }
	label="$vendor ($(echo "$vcommit" | cut -c1-12))"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
strip() { grep -vE '^[[:space:]]*#[[:space:]]*include[[:space:]]|^__FBSDID' || true; }

printf '%-14s %7s %7s %7s %7s %7s %5s\n' file freebsd ours common added removed xnu > "$tmp/sum"
tf=0; to=0; tc=0; ta=0; tr=0; tx=0
for f in $files; do
	if [ ! -f "$top/core/$f" ]; then
		printf '%-14s %s\n' "$f" "(not ported yet)" >> "$tmp/sum"
		continue
	fi
	upstream "$f" | strip > "$tmp/a"
	strip < "$top/core/$f" > "$tmp/b"
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
echo "== vs $label" \
     "(common = lines unchanged; added/removed = diff -u +/- lines; xnu = lines mentioning XNU)"
cat "$tmp/sum"
