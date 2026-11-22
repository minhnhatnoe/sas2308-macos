/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 */
/* count-only probes (no thread-local state, no drops), 10 s: gate acquisitions per IO by call path,
   family/kernel calls per IO, distinct tasks handed out by the family pool */
#pragma D option quiet
#pragma D option aggsize=16m
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN33IOSCSIParallelInterfaceController20CompleteParallelTask*:entry { @n["CompleteParallelTask"] = count(); }
fbt:mach_kernel:_ZN10IOWorkLoop9closeGateEv:entry { @wl[arg0] = count(); @g[arg0, stack(4)] = count(); }
fbt:mach_kernel:_ZN10IOWorkLoop12tryCloseGateEv:entry { @n["tryCloseGate"] = count(); @g[arg0, stack(4)] = count(); }
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer10SetTimeout*:entry { @n["SCSIParallelTimer::SetTimeout"] = count(); }
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer10RemoveTask*:entry { @n["SCSIParallelTimer::RemoveTask"] = count(); }
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN17SCSIParallelTimer5Rearm*:entry { @n["SCSIParallelTimer::Rearm"] = count(); }
fbt:com.apple.iokit.IOSCSIParallelFamily:*FindTaskFor*:entry { @n[probefunc] = count(); }
fbt:com.apple.iokit.IOSCSIParallelFamily:*GetSCSIParallelTask*:return { @task[arg1] = count(); }
fbt:mach_kernel:_ZN12IODMACommand7prepare*:entry { @n["IODMACommand::prepare"] = count(); }
fbt:mach_kernel:_ZN12IODMACommand8complete*:entry { @n["IODMACommand::complete"] = count(); }
fbt:mach_kernel:_ZN12IODMACommand15genIOVMSegments*:entry { @n["IODMACommand::genIOVMSegments"] = count(); }
fbt:mach_kernel:_ZN22IOInterruptEventSource12checkForWorkEv:entry { @cfw[arg0] = count(); }
fbt:mach_kernel:_ZN28IOFilterInterruptEventSource23normalInterruptOccurred*:entry { @irq[arg0] = count(); }
fbt:mach_kernel:thread_call_enter:entry, fbt:mach_kernel:thread_call_enter1:entry, fbt:mach_kernel:thread_call_enter_delayed:entry, fbt:mach_kernel:thread_call_cancel:entry { @tc[probefunc, caller] = count(); }
fbt:mach_kernel:IOLockLock:entry { @lk[caller] = count(); }
fbt:mach_kernel:_ZN17IOTimerEventSource*:entry { @n[probefunc] = count(); }
tick-10s { exit(0); }
END { trunc(@wl, 5); trunc(@g, 40); trunc(@cfw, 4); trunc(@irq, 3); trunc(@tc, 20); trunc(@lk, 25);
  printf("=== counts\n"); printa("%s %@d\n", @n);
  printf("=== closeGate by workloop\n"); printa("%p %@d\n", @wl);
  printf("=== checkForWork by source\n"); printa("%p %@d\n", @cfw);
  printf("=== irq by source\n"); printa("%p %@d\n", @irq);
  printf("=== thread_call by caller\n"); printa("%s %p %@d\n", @tc);
  printf("=== IOLockLock by caller\n"); printa("%p %@d\n", @lk);
  printf("=== distinct tasks\n"); printa("%p %@d\n", @task);
  printf("=== gate stacks\n"); printa("%p %k %@d\n", @g); }
