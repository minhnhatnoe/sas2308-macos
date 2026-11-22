# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
# find our kext's load address: return addresses of calls from our kext into the family, 5 s under a short load
. "$(dirname "$0")/lib_prof.sh"
setup || exit 1
$SB -b 4K -q 2 -t 12 $(jobs r outer 0 1) > /dev/null 2>&1 & SBP=$!; sleep 3
dtrace -Z -q -n 'fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN33IOSCSIParallelInterfaceController20CompleteParallelTask*:entry { @c[caller] = count(); }
 fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer10SetTimeout*:entry { @s[caller] = count(); }
 fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN33IOSCSIParallelInterfaceController14GetDMACommand*:entry,
 fbt:com.apple.iokit.IOSCSIParallelFamily:*GetDataBuffer*:entry { @d[probefunc, caller] = count(); }
 tick-5s { exit(0); } END { printf("CPT\n"); printa("%p %@d\n", @c); printf("SETTIMEOUT\n"); printa("%p %@d\n", @s); printf("OTHER\n"); printa("%s %p %@d\n", @d); }' > $OUT/base.txt 2>&1
wait $SBP
