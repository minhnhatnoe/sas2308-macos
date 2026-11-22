/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 */
/* mutex contention on the IO path, 20 s: spin/block time by lock and short stack */
#pragma D option quiet
#pragma D option aggsize=16m
lockstat:::adaptive-spin, lockstat:::adaptive-block { @t[probename, arg0] = sum(arg1); @n[probename, arg0] = count(); @s[probename, stack(8)] = sum(arg1); }
tick-20s { exit(0); }
END { trunc(@t, 15); trunc(@n, 15); trunc(@s, 15);
  printf("=== time\n"); printa("%s %p %@d\n", @t); printf("=== count\n"); printa("%s %p %@d\n", @n);
  printf("=== stacks\n"); printa(@s); }
