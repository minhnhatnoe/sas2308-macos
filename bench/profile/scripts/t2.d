/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 */
/* inner family/kernel calls on the IO path: per-call on-CPU ns and counts, 20 s */
#pragma D option quiet
#pragma D option aggsize=8m
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer10SetTimeout*:entry,
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer10RemoveTask*:entry,
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer5Rearm*:entry,
fbt:com.apple.iokit.IOSCSIParallelFamily:*FindTaskFor*:entry,
fbt:com.apple.iokit.IOSCSIParallelFamily:*OutstandingTaskList*:entry,
fbt:com.apple.iokit.IOSCSIParallelFamily:*GetSCSIParallelTask*:entry,
fbt:com.apple.iokit.IOSCSIParallelFamily:*FreeSCSIParallelTask*:entry,
fbt:mach_kernel:_ZN12IODMACommand7prepare*:entry,
fbt:mach_kernel:_ZN12IODMACommand8complete*:entry,
fbt:mach_kernel:_ZN12IODMACommand15genIOVMSegments*:entry,
fbt:mach_kernel:_ZN12IODMACommand20setMemoryDescriptor*:entry,
fbt:mach_kernel:_ZN12IODMACommand21clearMemoryDescriptor*:entry
{ self->t[probefunc] = vtimestamp; }
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer10SetTimeout*:return,
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer10RemoveTask*:return,
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer5Rearm*:return,
fbt:com.apple.iokit.IOSCSIParallelFamily:*FindTaskFor*:return,
fbt:com.apple.iokit.IOSCSIParallelFamily:*OutstandingTaskList*:return,
fbt:com.apple.iokit.IOSCSIParallelFamily:*GetSCSIParallelTask*:return,
fbt:com.apple.iokit.IOSCSIParallelFamily:*FreeSCSIParallelTask*:return,
fbt:mach_kernel:_ZN12IODMACommand7prepare*:return,
fbt:mach_kernel:_ZN12IODMACommand8complete*:return,
fbt:mach_kernel:_ZN12IODMACommand15genIOVMSegments*:return,
fbt:mach_kernel:_ZN12IODMACommand20setMemoryDescriptor*:return,
fbt:mach_kernel:_ZN12IODMACommand21clearMemoryDescriptor*:return
/self->t[probefunc]/
{ @n[probefunc] = count(); @v[probefunc] = sum(vtimestamp - self->t[probefunc]); self->t[probefunc] = 0; }
tick-20s { exit(0); }
END { printf("=== count\n"); printa("%s %@d\n", @n); printf("=== vns_sum\n"); printa("%s %@d\n", @v); }
