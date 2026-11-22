/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 */
/* on-CPU time and switches per thread, wakeups, 20 s */
#pragma D option quiet
#pragma D option aggsize=8m
sched:::on-cpu { self->on = timestamp; }
sched:::off-cpu /self->on/ { @t[execname, tid] = sum(timestamp - self->on); @n[execname, tid] = count(); self->on = 0; }
sched:::wakeup { @w[stringof(args[1]->pr_fname), execname] = count(); }
tick-20s { exit(0); }
END { trunc(@t, 40); trunc(@n, 40); trunc(@w, 30);
  printf("=== oncpu_ns\n"); printa("%s %d %@d\n", @t);
  printf("=== switches\n"); printa("%s %d %@d\n", @n);
  printf("=== wakeups (target, waker)\n"); printa("%s %s %@d\n", @w); }
