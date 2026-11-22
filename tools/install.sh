#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# Install, roll back or verify SAS2308HBA.kext on an Apple-silicon Mac.
#
#   tools/install.sh -n                  dry run: print every action, change nothing
#   tools/install.sh                     build kext/ (make, make check) and install it
#   tools/install.sh path/to/SAS2308HBA.kext   install a given bundle
#   tools/install.sh --rollback [path]   reinstall a backup (default: the newest
#                                        ~/sas2308-rollback-* that differs from the installed one)
#   tools/install.sh --verify [--expect N]   after the reboot: loaded UUID, drives, counters (read-only)
#
# Options: -n/--dry-run, -f/--force (reinstall the same UUID), --expect N, -h/--help.
# Run as your admin user; the script calls sudo (you type your password).
# Install steps: check SIP/boot policy, check the bundle, back up the
# installed bundle to ~/sas2308-rollback-<UUID8>, unmount volumes on the
# SAS2308HBA disks, copy to /Library/Extensions, chown root:wheel,
# kmutil load -p, then one approval in System Settings and a restart.
# Manual steps and recovery: docs/LOADING.md.
set -u
# shellcheck source=tools/install-lib.sh
. "$(dirname "$0")/install-lib.sh"

usage() { sed -n '2,17s/^# \{0,1\}//p' "$0"; exit "${1:-0}"; }

MODE=install FORCE=0 SRC="" EXPECT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -n|--dry-run) DRY=1 ;;
    -f|--force) FORCE=1 ;;
    --verify) MODE=verify ;;
    --expect) [ $# -ge 2 ] || usage 2; EXPECT=$2; shift ;;
    --rollback)
      MODE=rollback
      if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then SRC=$2; shift; fi ;;
    -h|--help) usage 0 ;;
    -*) say "unknown option $1" >&2; usage 2 ;;
    *) SRC=$1 ;;
  esac
  shift
done

verify() {
  local inst load disks n m rc=0 ctl stats cpf
  hdr "kext"
  inst=$( [ -d "$INSTALLED" ] && bundle_uuid "$INSTALLED" )
  load=$(loaded_uuid)
  say "installed: ${inst:-none} ($INSTALLED)"
  say "loaded:    ${load:-none}"
  if [ -z "$load" ]; then
    warn "SAS2308HBA is not loaded (it loads only when the card is present; after an install: approval and restart done?)"
    rc=1
  elif [ "$(loaded_id)" = "$OLD_BUNDLE_ID" ]; then
    warn "the loaded kext has the old bundle ID $OLD_BUNDLE_ID: install the current build and restart"
    rc=1
  elif [ -n "$inst" ] && [ "$inst" != "$load" ]; then
    warn "loaded UUID differs from the installed bundle: approve it in System Settings and/or restart"
    rc=1
  fi
  hdr "drives"
  disks=$(sas_whole_disks)
  n=$(printf '%s' "$disks" | grep -c . || true)
  for d in $disks; do
    say "  $d  $(diskutil info "$d" 2>/dev/null | sed -n 's/.*Media Name: *//p')  $(diskutil info "$d" 2>/dev/null | sed -n 's/^ *Disk Size: *\([^(]*\).*/\1/p')"
  done
  say "drives behind SAS2308HBA: $n${EXPECT:+ (expected $EXPECT)}"
  if [ -n "$EXPECT" ] && [ "$n" != "$EXPECT" ]; then rc=1; fi
  m=$(sas_mounts)
  if [ -n "$m" ]; then printf '%s\n' "$m" | sed 's/|/ on /; s/^/  mounted: \/dev\//'; else say "  no volumes mounted on them"; fi
  hdr "counters"
  ctl=$REPO/tools/sas2308ctl
  [ -x "$ctl" ] || ctl=$(command -v sas2308ctl || true)
  if [ -z "$ctl" ]; then
    warn "sas2308ctl not built (make -C $REPO/tools)"; rc=1
  elif [ -n "$load" ]; then
    stats=$("$ctl" stats 2>&1)
    printf '%s\n' "$stats" | grep -E '^ *(State|Config page (reads|retries|failures)|Controller dead|IOC faults|IOC temperature \(C\)|Diag resets|IO timeouts) '
    cpf=$(printf '%s\n' "$stats" | awk '/Config page failures/ { print $NF; exit }')
    if [ "${cpf:-x}" != 0 ]; then warn "config page failures: ${cpf:-not reported}"; rc=1; fi
  fi
  hdr "result"
  if [ "$rc" = 0 ]; then say "verify OK"; else say "verify FAILED (see warnings)"; fi
  return "$rc"
}

# Pick the bundle to install.
pick_source() {
  local inst
  case "$MODE" in
    install)
      if [ -n "$SRC" ]; then
        say "bundle given: $SRC"
      else
        SRC=$REPO/kext/build/$KEXT_NAME
        if [ "$DRY" = 1 ]; then
          # make relinks and re-signs on every run (the bundle directory is
          # the target); what matters is whether any unit recompiles.
          say "make would recompile $(make -n -C "$REPO/kext" 2>/dev/null | grep -c ' -c ') file(s) in $REPO/kext"
          act make -C "$REPO/kext"
          act make -C "$REPO/kext" check
          [ -d "$SRC" ] || { say "(no bundle yet at $SRC: the rest assumes the build succeeds)"; return 0; }
        else
          act make -C "$REPO/kext" || die "make failed"
          act make -C "$REPO/kext" check || die "make check failed"
        fi
      fi ;;
    rollback)
      inst=$( [ -d "$INSTALLED" ] && bundle_uuid "$INSTALLED" )
      if [ -z "$SRC" ]; then
        # newest first; backups live in ~/sas2308-rollback-<UUID8> (no spaces)
        # shellcheck disable=SC2012
        SRC=$(ls -td "$HOME"/sas2308-rollback-*/"$KEXT_NAME" 2>/dev/null | while read -r b; do
          u=$(bundle_uuid "$b")
          if [ -n "$u" ] && [ "$u" != "$inst" ]; then printf '%s' "$b"; break; fi
        done)
        [ -n "$SRC" ] || die "no backup in ~/sas2308-rollback-* that differs from the installed ${inst:-bundle}"
        say "newest backup that differs from the installed one: $SRC"
      fi
      [ -d "$SRC/$KEXT_NAME" ] && SRC=$SRC/$KEXT_NAME
      say "rolling back to: $SRC" ;;
  esac
  SRC=$(cd "$SRC" 2>/dev/null && pwd) || die "no bundle at $SRC"
  check_bundle "$SRC"
}

install() {
  local new inst out rc
  hdr "security"
  if ! check_security; then
    [ "$DRY" = 1 ] || exit 1
    say "[dry-run] a real run would stop here"
  fi
  hdr "bundle"
  pick_source
  new=$( [ -d "$SRC" ] && bundle_uuid "$SRC" )
  inst=$( [ -d "$INSTALLED" ] && bundle_uuid "$INSTALLED" )
  say "to install: ${new:-unknown until built}   installed: ${inst:-none}   loaded: $(loaded_uuid)"
  note_old_id
  if [ -n "$new" ] && [ "$new" = "$inst" ] && [ "$FORCE" = 0 ]; then
    say "$new is already installed; nothing to do (--force reinstalls it, --verify checks it)"
    return 0
  fi
  case "$SRC" in "$INSTALLED"|"$INSTALLED"/*) die "the source is the installed bundle itself" ;; esac
  hdr "backup"
  backup_installed
  hdr "SAS volumes"
  show_sas_disks
  unmount_sas || die "volumes on SAS2308HBA disks are still mounted; unmount them and rerun"
  hdr "install"
  [ -d "$INSTALLED" ] && act sudo rm -rf "$INSTALLED"
  act sudo cp -R "$SRC" /Library/Extensions/
  act sudo chown -R root:wheel "$INSTALLED"
  if [ "$DRY" = 1 ]; then
    act sudo kmutil load -p "$INSTALLED"
    say "[dry-run] then: approve in System Settings (if kmutil returns 27), restart, $0 --verify"
    return 0
  fi
  say "+ sudo kmutil load -p $INSTALLED"
  out=$(sudo kmutil load -p "$INSTALLED" 2>&1); rc=$?
  printf '%s\n' "$out"
  case "$rc" in
    0)
      if [ "$(loaded_uuid)" = "$new" ]; then say "loaded $new"
      else say "kmutil accepted the bundle; restart to replace the loaded $(loaded_uuid), then: $0 --verify"; fi ;;
    27)
      cat <<EOF

kmutil returned 27: macOS wants the new kext approved once.
  1. At the Mac (keyboard and mouse, or Screen Sharing): System Settings >
     Privacy & Security > "Allow" next to the SAS2308HBA / system software
     message. It asks for your password twice. Or open the pane with
       open "$SETTINGS_URL"
     (from a logged-in GUI session; over ssh it opens on the console).
  2. Click "Restart" when macOS asks (new system extensions need a restart).
     The volumes on the SAS disks stay unmounted until then; do not remount them.
  3. After the restart: $0 --verify
EOF
      ;;
    *) die "kmutil load -p failed with rc $rc (bundle installed at $INSTALLED; $0 --rollback restores the backup)" ;;
  esac
}

require_mac
[ "$DRY" = 1 ] && say "DRY RUN: nothing will be changed"
case "$MODE" in
  verify) verify ;;
  *) install ;;
esac
