#!/bin/bash
# Print "diskN SERIAL TGT" for every whole disk that qualifies as one of the
# SAS2308HBA Exos drives: descendant of SAS2308HBA in IORegistry AND diskutil
# Protocol SAS, model ST18000NM000J, size 18000207937536 bytes. Exit 1 if none.
set -u
out=()
while read -r d ser tgt; do
  case "$d" in disk0|disk1|disk2|disk3) continue;; esac
  info=$(diskutil info "$d" 2>/dev/null) || continue
  echo "$info" | grep -q 'Protocol: *SAS$' || continue
  echo "$info" | grep -q 'Media Name: *ST18000NM000J' || continue
  echo "$info" | grep -q '(18000207937536 Bytes)' || continue
  echo "$info" | grep -q 'Device Location: *External' || continue
  out+=("$d $ser $tgt")
done < <(ioreg -r -c SAS2308HBA -l | awk '
  /"SCSI Target Identifier"=/ { if (match($0,/"SCSI Target Identifier"=[0-9]+/)) { s=substr($0,RSTART,RLENGTH); sub(/.*=/,"",s); tgt=s } }
  /^[ |]*"Serial Number" = / { s=$0; sub(/.*= "/,"",s); sub(/".*/,"",s); gsub(/ /,"",s); ser=s }
  /"BSD Name" = "disk[0-9]+"/ { s=$0; sub(/.*= "/,"",s); sub(/".*/,"",s); print s, ser, tgt }')
[ ${#out[@]} -eq 0 ] && exit 1
printf '%s\n' "${out[@]}"
