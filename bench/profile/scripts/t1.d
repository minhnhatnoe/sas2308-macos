/* top-level per-call cost: submit (SendSCSICommand), completion (CompleteParallelTask),
   interrupt (filter count, checkForWork = work loop run incl. HandleInterruptRequest), 20 s */
#pragma D option quiet
#pragma D option aggsize=8m
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN29IOSCSIParallelInterfaceDevice15SendSCSICommand*:entry { self->s = vtimestamp; self->sw = timestamp; }
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN29IOSCSIParallelInterfaceDevice15SendSCSICommand*:return /self->s/ {
  @c["send"] = count(); @v["send"] = sum(vtimestamp - self->s); @wl["send"] = sum(timestamp - self->sw);
  @q["send vns"] = quantize(vtimestamp - self->s); self->s = 0; }
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN33IOSCSIParallelInterfaceController20CompleteParallelTask*:entry { self->c = vtimestamp; @cc[caller] = count(); }
fbt:com.apple.iokit.IOSCSIParallelFamily:_ZN33IOSCSIParallelInterfaceController20CompleteParallelTask*:return /self->c/ {
  @c["complete"] = count(); @v["complete"] = sum(vtimestamp - self->c); @q["complete vns"] = quantize(vtimestamp - self->c); self->c = 0; }
fbt:mach_kernel:_ZN28IOFilterInterruptEventSource23normalInterruptOccurred*:entry { @irq[arg0] = count(); }
fbt:mach_kernel:_ZN22IOInterruptEventSource12checkForWorkEv:entry { self->w = vtimestamp; self->wthis = arg0; }
fbt:mach_kernel:_ZN22IOInterruptEventSource12checkForWorkEv:return /self->w/ {
  @cfw[self->wthis] = count(); @cfwv[self->wthis] = sum(vtimestamp - self->w); self->w = 0; }
fbt:mach_kernel:thread_call_enter:entry, fbt:mach_kernel:thread_call_enter1:entry { @tce[caller] = count(); }
tick-20s { exit(0); }
END { trunc(@irq, 8); trunc(@cfw, 8); trunc(@cfwv, 8); trunc(@tce, 20); trunc(@cc, 10);
  printf("=== count\n"); printa("%s %@d\n", @c); printf("=== vns_sum\n"); printa("%s %@d\n", @v);
  printf("=== wall_sum\n"); printa("%s %@d\n", @wl);
  printf("=== irq (this)\n"); printa("%p %@d\n", @irq);
  printf("=== checkForWork count\n"); printa("%p %@d\n", @cfw); printf("=== checkForWork vns\n"); printa("%p %@d\n", @cfwv);
  printf("=== CompleteParallelTask callers\n"); printa("%p %@d\n", @cc);
  printf("=== thread_call_enter callers\n"); printa("%p %@d\n", @tce);
  printa(@q); }
