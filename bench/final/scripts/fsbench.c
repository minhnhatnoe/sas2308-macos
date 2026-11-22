/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 */
/* fsbench: QD1 sequential writes of BS bytes into FILE (wrapping at SIZE_MIB), F_NOCACHE, for SEC
 * seconds, with MODE after every write: 0 none, 1 fsync(), 2 fcntl(F_FULLFSYNC), 3 fcntl(F_BARRIERFSYNC).
 * The file is preallocated (written once) first. Latency = write + sync.
 * Prints: label mode ops iops MBps p50_us p99_us max_us errors
 * usage: fsbench FILE SIZE_MIB BS SEC MODE LABEL */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: fsbench FILE SIZE_MIB BS SEC MODE LABEL\n"); return 2; }
    const char *path = argv[1]; long size = atol(argv[2]) << 20, bs = atol(argv[3]);
    double sec = atof(argv[4]); int mode = atoi(argv[5]);
    char *buf; if (posix_memalign((void **)&buf, 16384, bs > (1 << 20) ? bs : (1 << 20))) return 1;
    memset(buf, 0x5a, bs > (1 << 20) ? bs : (1 << 20));
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    fcntl(fd, F_NOCACHE, 1);
    for (long o = 0; o < size; o += 1 << 20) if (pwrite(fd, buf, 1 << 20, o) != 1 << 20) { perror("prealloc"); return 1; }
    fcntl(fd, F_FULLFSYNC);
    long cap = 1 << 22, n = 0, err = 0; double *lat = malloc(cap * sizeof *lat), t0 = now();
    while (now() - t0 < sec) {
        double a = now();
        if (pwrite(fd, buf, bs, (n * bs) % size) != bs) err++;
        int r = 0;
        if (mode == 1) r = fsync(fd); else if (mode == 2) r = fcntl(fd, F_FULLFSYNC); else if (mode == 3) r = fcntl(fd, F_BARRIERFSYNC);
        if (r) err++;
        if (n < cap) lat[n] = (now() - a) * 1e6;
        n++;
    }
    double el = now() - t0; long m = n < cap ? n : cap;
    qsort(lat, m, sizeof *lat, cmp);
    printf("%s %d %ld %.0f %.1f %.0f %.0f %.0f %ld\n", argv[6], mode, n, n / el, n * (double)bs / el / 1e6,
           lat[m / 2], lat[(long)(m * 0.99)], lat[m - 1], err);
    close(fd); return 0;
}
