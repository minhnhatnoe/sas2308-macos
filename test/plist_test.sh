#!/bin/sh
# plist_test.sh - the kext personality matches every SAS2308 device id of
# FreeBSD's mps_pci.c (D52): SAS2308_1 0x0086, SAS2308_2 0x0087,
# SAS2308_3 0x006E (MPI2_MFGPAGE_DEVID_SAS2308_*), vendor 0x1000, and no
# other MPI2 chip.
plist=$(dirname "$0")/../kext/Info.plist
match=$(sed -n '/<key>IOPCIMatch<\/key>/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' "$plist")
want="0x00861000 0x00871000 0x006e1000"
got=$(for id in $match; do echo "$id"; done | tr 'A-F' 'a-f' | sort | tr '\n' ' ' | sed 's/ $//')
exp=$(for id in $want; do echo "$id"; done | sort | tr '\n' ' ' | sed 's/ $//')
if [ "$got" != "$exp" ]; then
	echo "FAIL plist IOPCIMatch: '$match', want '$want'"
	exit 1
fi
echo "plist D52: IOPCIMatch $match"

# D15a: Read/Write Time Out Duration are FreeBSD da's DA_DEFAULT_TIMEOUT (60 s)
for k in "Read Time Out Duration" "Write Time Out Duration"; do
	v=$(sed -n "/<key>$k<\/key>/{n;s/.*<integer>\(.*\)<\/integer>.*/\1/p;}" "$plist")
	if [ "$v" != "60000" ]; then
		echo "FAIL plist $k: '$v', want 60000 (FreeBSD da 60 s)"
		exit 1
	fi
done
echo "plist D15a: Read/Write Time Out Duration 60000"
