# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
#
# Sourced by the benchmark scripts that select drives with sasdisks.sh. Most of them WRITE to the
# raw disks (/dev/rdiskN) of every matching drive behind the HBA: all data on those drives is
# destroyed. confirm_destroy FILE (lines "diskN SERIAL TARGET") succeeds only if the environment
# variable SAS2308_YES_DESTROY names exactly those disks, comma-separated, in any order, e.g.
#   sudo SAS2308_YES_DESTROY=disk4,disk5,disk6,disk7,disk8,disk9,disk10 bash bench/scripts/phaseA.sh
confirm_destroy() {
  local want have
  want=$(awk '{print $1}' "$1" | sort | paste -s -d, -)
  have=$(printf '%s\n' "${SAS2308_YES_DESTROY:-}" | tr ', ' '\n\n' | grep -v '^$' | sort | paste -s -d, -)
  [ -n "$want" ] && [ "$want" = "$have" ] && return 0
  {
    echo "REFUSED: this benchmark writes to the raw disks below; all data on them is destroyed:"
    sed 's/^/  /' "$1"
    echo "To go ahead, name exactly these disks:"
    echo "  sudo SAS2308_YES_DESTROY=$want bash $0 ..."
  } >&2
  return 1
}
