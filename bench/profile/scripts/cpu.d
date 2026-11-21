/* where the work runs: samples by cpu for the work loop (in runEventSources/threadMain), sasbench, audiomxd, configd, idle; 10 s */
#pragma D option quiet
profile-997 { @c[cpu, execname == "kernel_task" ? (curthread == 0 ? "?" : "kernel_task") : execname] = count(); }
fbt:mach_kernel:_ZN10IOWorkLoop15runEventSourcesEv:entry /arg0 == $1/ { @wl[cpu] = count(); @wt[tid] = count(); }
tick-10s { exit(0); }
END { printf("=== cpu exec\n"); printa("%d %s %@d\n", @c); printf("=== wl runEventSources by cpu\n"); printa("%d %@d\n", @wl); printf("=== wl tid\n"); printa("%d %@d\n", @wt); }
