/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 */
/* kernel/user PC sampling, 20 s. $1/$2 = our kext's text range (0 0 if unknown). */
#pragma D option quiet
#pragma D option aggsize=32m
#pragma D option bufsize=8m
#pragma D option stackframes=40
profile-4997 { @all = count(); @cpu[cpu] = count(); }
profile-4997 /arg0 != 0/ {
  @k = count(); @kmod[mod(arg0)] = count(); @kfn[func(arg0)] = count();
  @kst[stack()] = count(); @kthr[execname, tid] = count();
}
profile-4997 /arg0 >= $1 && arg0 < $2/ { @ours[arg0] = count(); }
profile-4997 /arg0 == 0/ { @u[execname] = count(); }
tick-20s { exit(0); }
END {
  trunc(@kfn, 300); trunc(@kst, 600); trunc(@kthr, 60);
  printf("=== all\n"); printa("%@d\n", @all); printf("=== kernel\n"); printa("%@d\n", @k);
  printf("=== cpu\n"); printa("%d %@d\n", @cpu);
  printf("=== user\n"); printa("%s %@d\n", @u);
  printf("=== kthr\n"); printa("%s %d %@d\n", @kthr);
  printf("=== kmod\n"); printa("%a %@d\n", @kmod);
  printf("=== kfn\n"); printa("%a %@d\n", @kfn);
  printf("=== ours\n"); printa("%p %@d\n", @ours);
  printf("=== kst\n"); printa(@kst);
}
