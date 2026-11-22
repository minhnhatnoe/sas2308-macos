# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# shellcheck shell=bash
# Shared helpers for tools/install.sh and tools/uninstall.sh (sourced, not run).
# Written for macOS /bin/bash 3.2: no associative arrays, no mapfile.

BUNDLE_ID=org.sas2308.driver.SAS2308HBA
KEXT_NAME=SAS2308HBA.kext
KEXT_BIN=Contents/MacOS/SAS2308HBA
INSTALLED=/Library/Extensions/$KEXT_NAME
STAGED=/Library/StagedExtensions/Library/Extensions/$KEXT_NAME
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SETTINGS_URL='x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Security'
DRY=0

say()  { printf '%s\n' "$*"; }
hdr()  { printf '\n== %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

quote() {
  local out="" a
  for a in "$@"; do out="$out $(printf '%q' "$a")"; done
  printf '%s' "${out# }"
}

# Run a command that changes the system; under --dry-run only print it.
act() {
  if [ "$DRY" = 1 ]; then
    printf '[dry-run] would run: %s\n' "$(quote "$@")"
  else
    printf '+ %s\n' "$(quote "$@")"
    "$@"
  fi
}

require_mac() {
  [ "$(uname -s)" = Darwin ] || die "macOS only (this is $(uname -s))"
  [ "$(uname -m)" = arm64 ] || die "Apple silicon only (this is $(uname -m))"
  [ "$(id -u)" != 0 ] || die "run as your admin user, not as root: the script calls sudo itself"
  say "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m)"
}

# UUID of a bundle's binary, or empty.
bundle_uuid() {
  dwarfdump --uuid "$1/$KEXT_BIN" 2>/dev/null | awk '/^UUID:/ { print $2; exit }'
}

# UUID of the loaded kext, or empty.
loaded_uuid() {
  kmutil showloaded --list-only 2>/dev/null | awk -v id="$BUNDLE_ID" '
    index($0, id) { for (i = 1; i <= NF; i++) if (length($i) == 36 && $i ~ /^[0-9A-F-]+$/) { print $i; exit } }'
}

uuid8() { printf '%s' "${1%%-*}"; }

# Static checks on a bundle, the same ones `make check` runs on the build
# (Info.plist lint, arm64e binary, codesign) plus the bundle ID. Read-only.
check_bundle() {
  local b=$1 id archs
  [ -d "$b" ] || die "no bundle at $b"
  [ -f "$b/$KEXT_BIN" ] || die "$b has no $KEXT_BIN"
  plutil -lint "$b/Contents/Info.plist" >/dev/null || die "$b: Info.plist does not lint"
  id=$(plutil -extract CFBundleIdentifier raw "$b/Contents/Info.plist" 2>/dev/null)
  [ "$id" = "$BUNDLE_ID" ] || die "$b: bundle ID '$id', expected $BUNDLE_ID"
  archs=$(lipo -archs "$b/$KEXT_BIN" 2>/dev/null)
  case " $archs " in *" arm64e "*) ;; *) die "$b: binary is '$archs', not arm64e" ;; esac
  codesign --verify "$b" 2>/dev/null || die "$b: codesign --verify fails (rebuild with make)"
  say "bundle OK: $b (UUID $(bundle_uuid "$b"), $archs, $(codesign -dv "$b" 2>&1 | sed -n 's/^Signature=//p') signature)"
}

# SIP and boot policy needed for an ad-hoc signed third-party kext.
# Returns 1 (after explaining) when something is missing.
check_security() {
  local sip bp mode kexts auxp ok=1
  sip=$(csrutil status 2>&1)
  say "csrutil: $sip"
  case "$sip" in *disabled*) ;; *) ok=0 ;; esac
  say "reading the boot policy (sudo bputil -d)"
  bp=$(sudo bputil -d 2>/dev/null) || { warn "sudo bputil -d failed"; bp=""; }
  mode=$(printf '%s\n' "$bp" | sed -n 's/^Security Mode: *\([A-Za-z]*\).*/\1/p' | head -1)
  kexts=$(printf '%s\n' "$bp" | sed -n 's/^3rd Party Kexts Status: *\([A-Za-z]*\).*/\1/p' | head -1)
  auxp=$(printf '%s\n' "$bp" | sed -n 's/.*(auxp): *//p' | head -1)
  auxp=${auxp:+${auxp:0:16}...}
  say "boot policy: Security Mode ${mode:-?}, 3rd Party Kexts ${kexts:-?}, auxp ${auxp:-absent}"
  [ "$mode" = Permissive ] || ok=0
  [ "$kexts" = Enabled ] || ok=0
  [ "$ok" = 1 ] && return 0
  cat >&2 <<'EOF'
The Mac is not set up to load an ad-hoc signed kext. Needed once, at the
Mac's keyboard (docs/LOADING.md "One-time security change"):
  1. Shut down; hold the power button until "Loading startup options";
     Options > Continue (Recovery).
  2. Utilities > Terminal: csrutil disable   (gives Permissive Security)
  3. Utilities > Startup Security Utility > the disk > Security Policy:
     Reduced Security + "Allow user management of kernel extensions from
     identified developers".
  4. Restart. Expect: csrutil status "disabled"; sudo bputil -d shows
     Security Mode Permissive and 3rd Party Kexts Enabled.
This lowers the Mac's protection system-wide.
EOF
  return 1
}

# Whole disk of a BSD name: disk9s2 -> disk9.
whole_disk() { printf '%s' "$1" | sed -E 's/^(disk[0-9]+).*/\1/'; }

# BSD names (whole disks, partitions, APFS volumes) below SAS2308HBA in the
# IORegistry, minus anything on an internal disk or on the boot disk.
sas_bsd_names() {
  local n w boot internal
  boot=$(whole_disk "$(df / | awk 'NR == 2 { sub("^/dev/", "", $1); print $1 }')")
  ioreg -r -c SAS2308HBA -l -w0 2>/dev/null |
    sed -n 's/.*"BSD Name" = "\(disk[0-9s]*\)".*/\1/p' | sort -u |
    while read -r n; do
      w=$(whole_disk "$n")
      internal=$(diskutil info -plist "$w" 2>/dev/null </dev/null | plutil -extract Internal raw - 2>/dev/null)
      if [ "$w" = "$boot" ] || [ "$internal" != false ]; then
        warn "skipping $n: $w is internal or the boot disk (Internal=${internal:-?})"
        continue
      fi
      printf '%s\n' "$n"
    done
}

# Whole SAS disks below SAS2308HBA.
sas_whole_disks() { sas_bsd_names | grep -E '^disk[0-9]+$'; }

# "device|mount point" for every mounted volume on a SAS2308HBA disk.
sas_mounts() {
  local names
  names=$(sas_bsd_names)
  [ -n "$names" ] || return 0
  mount | sed -n 's#^/dev/\(disk[0-9s]*\) on \(.*\) (.*#\1|\2#p' |
    while IFS='|' read -r dev mp; do
      if printf '%s\n' "$names" | grep -qx "$dev"; then printf '%s|%s\n' "$dev" "$mp"; fi
    done
}

show_sas_disks() {
  local disks n
  disks=$(sas_whole_disks)
  n=$(printf '%s' "$disks" | grep -c . || true)
  say "disks behind SAS2308HBA: $n${disks:+ ($(printf '%s' "$disks" | tr '\n' ' ' | sed 's/ $//'))}"
}

# Unmount every volume on a SAS2308HBA disk. Fails if any stays mounted.
unmount_sas() {
  local m dev mp left
  m=$(sas_mounts)
  if [ -z "$m" ]; then say "no mounted volumes on SAS2308HBA disks"; return 0; fi
  while IFS='|' read -r dev mp <&3; do
    say "mounted on a SAS2308HBA disk: /dev/$dev on $mp"
    act sudo diskutil unmount "/dev/$dev" || warn "unmount of /dev/$dev failed"
  done 3<<<"$m"
  [ "$DRY" = 1 ] && return 0
  left=$(sas_mounts)
  if [ -n "$left" ]; then
    printf '%s\n' "$left" | sed 's/|/ on /; s/^/still mounted: \/dev\//' >&2
    return 1
  fi
  return 0
}

# Copy the installed bundle to ~/sas2308-rollback-<UUID8> unless that
# backup already exists.
backup_installed() {
  local u dest
  if [ ! -d "$INSTALLED" ]; then say "nothing installed at $INSTALLED: no backup needed"; return 0; fi
  u=$(bundle_uuid "$INSTALLED")
  [ -n "$u" ] || u=$(loaded_uuid)
  [ -n "$u" ] || die "cannot read the UUID of $INSTALLED (dwarfdump) or of the loaded kext"
  dest=$HOME/sas2308-rollback-$(uuid8 "$u")
  if [ -d "$dest/$KEXT_NAME" ]; then
    if [ "$(bundle_uuid "$dest/$KEXT_NAME")" = "$u" ]; then
      say "installed $u is already backed up in $dest"
      return 0
    fi
    die "$dest/$KEXT_NAME exists but is not $u; move it away first"
  fi
  say "backing up installed $u to $dest"
  act mkdir -p "$dest"
  act cp -R "$INSTALLED" "$dest/"
}
