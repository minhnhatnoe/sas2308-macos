#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
#
# Fetch the FreeBSD source the port follows into ref/freebsd-src (not part of
# the repository; ref/ is ignored by git).
#
#   tools/fetch-freebsd-ref.sh                  the Release: tag of core/VENDOR.txt
#   tools/fetch-freebsd-ref.sh sys/sys ...      also check out these upstream paths
#   tools/fetch-freebsd-ref.sh -t <tag> [path ...]   another tag (release/X.Y.0)
#
# Missing ref/freebsd-src: a shallow, blobless, sparse clone of the tag with
# sys/dev/mps and sys/cam checked out (about 24 MB):
#   git clone --depth 1 --filter=blob:none --sparse --branch <tag> <url> ref/freebsd-src
#   git -C ref/freebsd-src sparse-checkout set sys/dev/mps sys/cam [path ...]
# Present: fetches the tag (depth 1 when the clone is shallow), checks it out
# (detached HEAD) and adds the given paths to the sparse checkout.
# tools/diff-vs-freebsd.sh (REF=) and tools/import-freebsd.sh use this tree.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
url=https://github.com/freebsd/freebsd-src
ref=$top/ref/freebsd-src
die() { echo "fetch-freebsd-ref: $*" >&2; exit 1; }
usage() { sed -n '5,11s/^# \{0,1\}//p' "$0" >&2; exit 2; }

tag=""
while [ $# -gt 0 ]; do
	case $1 in
	-t) [ $# -ge 2 ] || usage; tag=$2; shift 2 ;;
	-h|--help) usage ;;
	-*) usage ;;
	*) break ;;
	esac
done
if [ -z "$tag" ]; then
	tag=$(sed -n 's/^Release: *\(release\/[^ ]*\).*/\1/p' "$top/core/VENDOR.txt")
	[ -n "$tag" ] || die "no 'Release: release/X.Y.0' line in core/VENDOR.txt; give one with -t"
fi

# exit 2: the tag does not exist upstream; anything else: no connection
rc=0
git ls-remote --exit-code --tags "$url" "refs/tags/$tag" >/dev/null || rc=$?
case $rc in
0) ;;
2) die "tag $tag not found in $url" ;;
*) die "cannot reach $url (network down?), git ls-remote exit $rc" ;;
esac

if [ ! -e "$ref" ]; then
	mkdir -p "$top/ref"
	echo "cloning $tag into ref/freebsd-src (shallow, blobless, sparse)"
	git -c advice.detachedHead=false clone -q --depth 1 --filter=blob:none --sparse --branch "$tag" "$url" "$ref" ||
		die "git clone of $tag failed"
	git -C "$ref" sparse-checkout set sys/dev/mps sys/cam "$@" ||
		die "sparse-checkout set failed"
else
	[ -d "$ref/.git" ] || die "ref/freebsd-src exists but is not a git clone; move it away"
	echo "fetching $tag into ref/freebsd-src"
	depth=""
	[ "$(git -C "$ref" rev-parse --is-shallow-repository)" = true ] && depth=--depth=1
	git -C "$ref" fetch -q $depth --filter=blob:none origin "refs/tags/$tag:refs/tags/$tag" ||
		die "git fetch of $tag failed"
	git -C "$ref" checkout -q --detach "refs/tags/$tag^{commit}" ||
		die "checkout of $tag failed (local changes in ref/freebsd-src?)"
	if [ $# -gt 0 ]; then
		[ "$(git -C "$ref" config --bool core.sparseCheckout)" = true ] ||
			die "ref/freebsd-src is not a sparse checkout (it has every path already)"
		git -C "$ref" sparse-checkout add "$@" || die "sparse-checkout add failed"
	fi
fi
echo "ref/freebsd-src: $tag ($(git -C "$ref" rev-parse --short=12 HEAD)), $(du -sh "$ref" | cut -f1)"
