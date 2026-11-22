#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# Remove SAS2308HBA.kext from an Apple-silicon Mac.
#
#   tools/uninstall.sh -n     dry run: print every action, change nothing
#   tools/uninstall.sh        uninstall
#
# Steps: back up the installed bundle to ~/sas2308-rollback-<UUID8> (so
# tools/install.sh --rollback can bring it back), unmount the volumes on the
# SAS2308HBA disks, kmutil unload (refused while any of them is mounted),
# remove /Library/Extensions/SAS2308HBA.kext, kmutil clear-staging when a
# copy is staged, then a restart drops it from the Auxiliary Kernel Collection.
# Run as your admin user; the script calls sudo (you type your password).
set -u
# shellcheck source=tools/install-lib.sh
. "$(dirname "$0")/install-lib.sh"

usage() { sed -n '2,12s/^# \{0,1\}//p' "$0"; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    -n|--dry-run) DRY=1 ;;
    -h|--help) usage 0 ;;
    *) say "unknown argument $1" >&2; usage 2 ;;
  esac
  shift
done

require_mac
[ "$DRY" = 1 ] && say "DRY RUN: nothing will be changed"

inst=$( [ -d "$INSTALLED" ] && bundle_uuid "$INSTALLED" )
load=$(loaded_uuid) load_id=$(loaded_id)
staged=0; [ -d "$STAGED" ] && staged=1
say "installed: ${inst:-none}   loaded: ${load:-none}   staged: $([ $staged = 1 ] && echo yes || echo no)"
note_old_id
if [ ! -d "$INSTALLED" ] && [ -z "$load" ] && [ $staged = 0 ]; then
  say "SAS2308HBA is not installed, loaded or staged: nothing to do"
  exit 0
fi

hdr "backup"
backup_installed

hdr "SAS volumes"
show_sas_disks
unmount_sas || die "volumes on SAS2308HBA disks are still mounted: not unloading"

hdr "unload"
unloaded=0
if [ -n "$load" ]; then
  if [ "$DRY" = 1 ]; then
    say "[dry-run] a real run refuses to unload if any volume above is still mounted after unmounting"
    act sudo kmutil unload -b "$load_id"
  elif [ -n "$(sas_mounts)" ]; then
    die "a volume on a SAS2308HBA disk is mounted: not unloading"
  elif act sudo kmutil unload -b "$load_id"; then
    unloaded=1
  else
    warn "kmutil unload failed (driver busy?); it stays loaded until the restart"
  fi
else
  say "not loaded"
fi

hdr "remove"
[ -d "$INSTALLED" ] && act sudo rm -rf "$INSTALLED"
if [ $staged = 1 ]; then
  act sudo kmutil clear-staging
else
  say "no staged copy at $STAGED: kmutil clear-staging not needed"
fi

hdr "next"
cat <<EOF
Restart the Mac (sudo shutdown -r now) so macOS rebuilds the Auxiliary
Kernel Collection without SAS2308HBA$([ $unloaded = 1 ] && echo " (it is already unloaded now)").
After the restart, kmutil showloaded --list-only | grep -i sas2308 should
print nothing, and the SAS disks are gone until a driver is installed again.
The SIP / boot policy changes stay; docs/LOADING.md says how to restore Full
Security. Reinstall the backup with: tools/install.sh --rollback
EOF
