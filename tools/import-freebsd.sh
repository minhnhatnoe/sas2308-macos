#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
#
# Import a FreeBSD sys/dev/mps release onto the vendor branch (core/VENDOR.txt).
#
#   tools/import-freebsd.sh release/14.5.0        a release tag (the policy)
#   tools/import-freebsd.sh <commit> <name>       any commit; tag vendor/freebsd-mps/<name>
#
# 1. Fetches the commit into ref/freebsd-src (blobless; cloned by
#    tools/fetch-freebsd-ref.sh if missing) with the history since the current
#    release (--shallow-exclude), plus one generation, so the log of step 4 is
#    complete and has no shallow artifacts.
# 2. In a temporary worktree of vendor/freebsd-mps, copies every file of the
#    path map in core/VENDOR.txt from that commit (`git show`, no checkout),
#    removes core/mpi/ files upstream dropped, and updates the header of
#    core/VENDOR.txt (Commit, Date, Release, Tag).
# 3. Warns about upstream files that are neither mapped nor listed as not ported.
# 4. Lists the upstream commits that touched the files we do not port, plus a
#    `git diff --stat` of them (the cross-check that needs no history).
# 5. Commits "Import FreeBSD sys/dev/mps at <short> (<name>)". Without such
#    commits it also creates the annotated tag and removes the worktree; with
#    them it leaves the worktree so the message's "decision:" lines can be
#    filled in (git commit --amend) and prints the tag command.
# Then merge vendor/freebsd-mps into a branch from master (core/VENDOR.txt,
# "Updating").
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
url=https://github.com/freebsd/freebsd-src
ref=$top/ref/freebsd-src
vendor=vendor/freebsd-mps
[ $# -ge 1 ] || { sed -n '5,8p' "$0" >&2; exit 2; }
spec=$1
case $spec in
release/*) name=${2:-${spec#release/}} ;;
*) [ $# -ge 2 ] || { echo "a commit needs a tag name: $0 <commit> <name>" >&2; exit 2; }
   name=$2 ;;
esac
tag=$vendor/$name
git -C "$top" rev-parse -q --verify "refs/tags/$tag" >/dev/null &&
	{ echo "tag $tag exists" >&2; exit 1; }

vtxt() { git -C "$top" show "$vendor:core/VENDOR.txt"; }
old=$(vtxt | sed -n 's/^Commit: *\([0-9a-f]\{40\}\).*/\1/p')
oldrel=$(vtxt | sed -n 's/^Release: *\(release\/[^ ]*\).*/\1/p')
[ -n "$old" ] || { echo "no Commit: line in $vendor:core/VENDOR.txt" >&2; exit 1; }

# 1. fetch
# a missing ref/freebsd-src is cloned at the current release (an empty
# blobless repository when the vendor branch is at a commit, not a release)
if [ ! -d "$ref/.git" ] && [ -n "$oldrel" ]; then
	"$top/tools/fetch-freebsd-ref.sh" -t "$oldrel"
elif [ ! -d "$ref/.git" ]; then
	git init -q "$ref"
	git -C "$ref" remote add origin "$url"
	git -C "$ref" config remote.origin.promisor true
	git -C "$ref" config remote.origin.partialclonefilter blob:none
fi
case $spec in
release/*) want="refs/tags/$spec:refs/tags/$spec" ;;
*) want=$spec ;;
esac
git -C "$ref" fetch -q --filter=blob:none --depth=1 origin "$old"
if [ -n "$oldrel" ]; then
	# the commits reachable from the new one but not from the old release
	git -C "$ref" fetch -q --filter=blob:none --shallow-exclude="$oldrel" origin "$want" || {
		echo "fetch failed; if ref/freebsd-src already has $spec as a shallow tip" \
			"(tools/fetch-freebsd-ref.sh -t), move ref/freebsd-src away and rerun" >&2
		exit 1; }
else
	since=$(git -C "$ref" log -1 --format=%cs "$old")
	git -C "$ref" fetch -q --filter=blob:none --shallow-since="$since" origin "$want"
fi
# one more generation, so the oldest fetched commits have their parents and
# their diffs are real (a commit without parents looks like it adds every file)
git -C "$ref" fetch -q --filter=blob:none --deepen=1 origin "$want"
new=$(git -C "$ref" rev-parse "FETCH_HEAD^{commit}")
short=$(echo "$new" | cut -c1-12)
[ "$new" != "$old" ] || { echo "already at $new" >&2; exit 1; }

# 2. copy into a vendor-branch worktree
wt=$(mktemp -d "${TMPDIR:-/tmp}/sas2308-vendor.XXXXXX")
rmdir "$wt"
git -C "$top" worktree add -q "$wt" "$vendor"
vtxt > "$wt/.vendor.txt"
mapped=""
while read -r up ours; do
	case $up in
	*/) for f in $(git -C "$ref" ls-tree --name-only "$new" "$up"); do
		git -C "$ref" show "$new:$f" > "$wt/$ours${f#"$up"}"
	    done
	    for f in $(cd "$wt" && git ls-files "$ours"); do
		git -C "$ref" cat-file -e "$new:$up${f#"$ours"}" 2>/dev/null ||
			git -C "$wt" rm -q "$f"
	    done ;;
	*)  git -C "$ref" show "$new:$up" > "$wt/$ours" ;;
	esac
	mapped="$mapped $up"
done <<EOF
$(sed -n 's|^\(sys/dev/mps/[^ ]*\) *-> *\(core/[^ ]*\).*|\1 \2|p' "$wt/.vendor.txt")
EOF
unported=$(sed -n '/^Upstream files not ported/,/^$/s|^\(sys/dev/mps/[^ ]*\).*|\1|p' "$wt/.vendor.txt" | tr '\n' ' ')
rm "$wt/.vendor.txt"
date=$(git -C "$ref" log -1 --format=%cs "$new")
sed -i.bak -e "s|^Commit: .*|Commit:    $new|" -e "s|^Date: .*|Date:      $date|" \
	-e "s|^Release: .*|Release:   $(case $spec in release/*) echo "$spec";; *) echo "-";; esac)|" \
	-e "s|^Tag: .*|Tag:       $tag|" "$wt/core/VENDOR.txt"
rm "$wt/core/VENDOR.txt.bak"

# 3. upstream files we do not know
for f in $(git -C "$ref" ls-tree --name-only "$new" sys/dev/mps/); do
	case " $mapped $unported " in
	*" $f "*|*" $f/ "*) ;;
	*) echo "warning: new upstream file $f: add it to the map or to 'not ported' in core/VENDOR.txt" >&2 ;;
	esac
done

# 4. upstream changes to the files we do not port
# commits after the shallow boundary (= not reachable from the old release)
log=$(git -C "$ref" log --format='%h %s' "$new" --not $(cat "$ref/.git/shallow") -- $unported)
stat=$(git -C "$ref" diff --stat "$old" "$new" -- $unported)

# 5. commit, tag
msg=$(mktemp)
{
	echo "Import FreeBSD sys/dev/mps at $short ($name)"
	echo
	echo "Pristine upstream files from $url"
	echo "$new ($date), mapped as in core/VENDOR.txt."
	if [ -n "$log$stat" ]; then
		echo
		echo "Upstream changes to files we do not port (decision for each):"
		echo "$log" | sed 's/^/  /; s/$/\n    decision: /'
		echo
		echo "$stat" | sed 's/^/  /'
	fi
} > "$msg"
git -C "$wt" add -A core
git -C "$wt" commit -q -F "$msg"
rm -f "$msg"
git -C "$top" diff --stat "$vendor~1" "$vendor" | tail -1
if [ -n "$log$stat" ]; then
	echo "Upstream changed files we do not port. Fill in the decisions, then:"
	echo "  git -C $wt commit --amend"
	echo "  git -C $top tag -a $tag -m 'FreeBSD sys/dev/mps $new ($date)' $vendor"
	echo "  git -C $top worktree remove $wt"
else
	git -C "$top" tag -a "$tag" -m "FreeBSD sys/dev/mps $new ($date), $url" "$vendor"
	git -C "$top" worktree remove "$wt"
	echo "committed and tagged $tag; now merge $vendor into a branch from master"
fi
