/*
 * sasbench: raw-disk benchmark and data-integrity checker for the SAS2308HBA kext.
 *
 * Each "job" is one device + mode + zone driven by N threads (= queue depth).
 * Several jobs may run at once (also on the same device, e.g. outer + inner).
 *
 *   sasbench [options] JOB [JOB ...]
 *   JOB  = path[,mode[,zone[,name]]]
 *     mode  r | w | m (mix, -M % reads) | v (verify: write region, then read back + compare)
 *           | c (check only: read back + compare a region written earlier with the same -s/-b/-B/zone)
 *     zone  outer (0-2%) | middle (49-51%) | inner (98-100%) | full | A-B (percent of capacity)
 *     name  stamp name for verify mode (drive serial); default = path
 *
 * Options (apply to every job):
 *   -b SIZE   block size (default 1M; suffix K/M)       -q N   threads per job (QD, default 1)
 *   -p seq|rand  access pattern (default seq; rand in verify = permuted order, each block once)
 *   -t SEC    duration limit (default 30)               -B SIZE  byte limit per job (suffix K/M/G)
 *   -M PCT    read percentage for mode m (default 70)   -s SEED  stamp seed (verify)
 *   -F 0|1|2  after every write: 1 = fcntl F_FULLFSYNC, 2 = ioctl DKIOCSYNCHRONIZECACHE
 *   -o FILE   append one CSV line per job + aggregate    -l LABEL  label for CSV lines
 *   -T FILE   interval CSV (time series), -i SEC interval (default 10)
 *   -n        dry run: print the resolved jobs (offsets) and exit
 *
 * Raw devices (/dev/rdiskN) are uncached; F_NOCACHE is set anyway. Buffers are page aligned.
 * Verify stamp per 4 KiB block: magic, 4K-block number, seed, name, then a payload derived
 * from (seed, block, name). A mismatch is classified (wrong block = misdirected/stale request,
 * wrong name = other drive's data, wrong seed = stale data from an earlier pass, payload).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/disk.h>
#endif

#define STAMP 4096
#define MAGIC 0x484345424e534153ULL /* "SASBENCH" little endian */
#define NBUCKET 1400
#define MAXJOBS 64
#define MAXMISLOG 20

enum { M_READ, M_WRITE, M_MIX, M_VERIFY, M_CHECK };

typedef struct {
    char path[256], name[32], zone[32];
    int mode, fd;
    uint64_t start, nblk;        /* window in bytes / blocks of bs */
    _Atomic uint64_t cursor;     /* seq / permuted index */
    uint64_t stride;             /* permutation stride (rand verify) */
    int phase;                   /* verify: 0 write, 1 read */
    /* stats (protected by lock, merged at thread exit) */
    pthread_mutex_t lock;
    uint64_t hist[NBUCKET];
    uint64_t rd_ops, wr_ops, rd_bytes, wr_bytes, errors, maxlat;
    uint64_t mismatches;
    int mislogged;
    double wphase_secs, rphase_secs; /* verify */
    _Atomic uint64_t live_bytes, live_ops, live_max;
    _Atomic int stop, writers_left;
    uint64_t bytes_cap;
    double secs;
} job_t;

static uint64_t g_bs = 1 << 20, g_bytes = 0, g_seed = 1;
static int g_qd = 1, g_rand = 0, g_readpct = 70, g_fsync = 0, g_dry = 0;
static double g_dur = 30, g_ival = 10;
static const char *g_csv, *g_tcsv, *g_label = "";
static job_t g_jobs[MAXJOBS];
static int g_njobs;
static _Atomic int g_done;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t parse_size(const char *s) {
    char *e;
    double v = strtod(s, &e);
    switch (*e) {
    case 'k': case 'K': v *= 1024; break;
    case 'm': case 'M': v *= 1048576; break;
    case 'g': case 'G': v *= 1073741824.0; break;
    case 't': case 'T': v *= 1099511627776.0; break;
    }
    return (uint64_t)v;
}

static int bucket(uint64_t us) {
    if (us < 64) return (int)us;
    int e = 63 - __builtin_clzll(us);
    int b = 64 + (e - 6) * 32 + (int)((us >> (e - 5)) & 31);
    return b < NBUCKET ? b : NBUCKET - 1;
}
static uint64_t bucket_val(int b) {
    if (b < 64) return (uint64_t)b;
    int e = (b - 64) / 32 + 6, sub = (b - 64) % 32;
    return (uint64_t)(32 + sub) << (e - 5);
}
static uint64_t pct(const uint64_t *h, double p) {
    uint64_t n = 0, acc = 0;
    for (int i = 0; i < NBUCKET; i++) n += h[i];
    if (!n) return 0;
    uint64_t want = (uint64_t)ceil(p * n);
    if (want < 1) want = 1;
    for (int i = 0; i < NBUCKET; i++)
        if ((acc += h[i]) >= want) return bucket_val(i);
    return 0;
}

static inline uint64_t xs(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}
static uint64_t hname(const char *n) {
    uint64_t h = 1469598103934665603ULL;
    while (*n) h = (h ^ (uint8_t)*n++) * 1099511628211ULL;
    return h;
}

typedef struct { uint64_t magic, blk, seed; char name[24]; } stamp_t;

static void fill_stamp(uint8_t *p, uint64_t blk, uint64_t seed, const char *name, uint64_t hn) {
    stamp_t *st = (stamp_t *)p;
    st->magic = MAGIC; st->blk = blk; st->seed = seed;
    memset(st->name, 0, sizeof st->name);
    strncpy(st->name, name, sizeof st->name - 1);
    uint64_t s = (seed * 0x9E3779B97F4A7C15ULL) ^ (blk * 0xC2B2AE3D27D4EB4FULL) ^ hn;
    if (!s) s = 1;
    uint64_t *w = (uint64_t *)(p + sizeof(stamp_t));
    for (size_t i = 0; i < (STAMP - sizeof(stamp_t)) / 8; i++) w[i] = xs(&s);
}

static void mismatch(job_t *j, uint64_t off, const uint8_t *got, const uint8_t *exp) {
    const stamp_t *g = (const stamp_t *)got, *e = (const stamp_t *)exp;
    const char *kind;
    if (g->magic != MAGIC) kind = "no-stamp(never written/garbage)";
    else if (strncmp(g->name, e->name, sizeof g->name)) kind = "other-drive-data";
    else if (g->blk != e->blk) kind = "wrong-block(misdirected/stale request)";
    else if (g->seed != e->seed) kind = "stale-data(old seed)";
    else kind = "payload-corrupt";
    size_t first = 0, nbad = 0;
    for (size_t i = 0; i < STAMP; i++)
        if (got[i] != exp[i]) { if (!nbad) first = i; nbad++; }
    pthread_mutex_lock(&j->lock);
    j->mismatches++;
    if (j->mislogged++ < MAXMISLOG) {
        char gn[25] = {0};
        memcpy(gn, g->name, 24);
        fprintf(stderr,
                "MISMATCH job=%s name=%s byte_off=%" PRIu64 " lba512=%" PRIu64 " blk4k=%" PRIu64
                " kind=%s first_bad_byte=%zu bad_bytes=%zu found{magic=%016" PRIx64 " blk=%" PRIu64
                " seed=%" PRIu64 " name=%s}\n",
                j->path, j->name, off, off / 512, off / STAMP, kind, first, nbad, g->magic, g->blk,
                g->seed, gn);
    }
    pthread_mutex_unlock(&j->lock);
}

static uint64_t gcd(uint64_t a, uint64_t b) { while (b) { uint64_t t = a % b; a = b; b = t; } return a; }

static int do_io(job_t *j, int wr, void *buf, uint64_t off) {
    ssize_t r = wr ? pwrite(j->fd, buf, g_bs, (off_t)off) : pread(j->fd, buf, g_bs, (off_t)off);
    if (r != (ssize_t)g_bs) {
        pthread_mutex_lock(&j->lock);
        if (j->errors++ < 10)
            fprintf(stderr, "IOERR job=%s %s off=%" PRIu64 " ret=%zd errno=%d (%s)\n", j->path,
                    wr ? "write" : "read", off, r, errno, strerror(errno));
        pthread_mutex_unlock(&j->lock);
        return -1;
    }
#ifdef F_FULLFSYNC
    if (wr && g_fsync == 1 && fcntl(j->fd, F_FULLFSYNC) < 0) {
        pthread_mutex_lock(&j->lock);
        if (j->errors++ < 10) fprintf(stderr, "F_FULLFSYNC failed: %s\n", strerror(errno));
        pthread_mutex_unlock(&j->lock);
    }
#endif
#ifdef DKIOCSYNCHRONIZECACHE
    if (wr && g_fsync == 2 && ioctl(j->fd, DKIOCSYNCHRONIZECACHE) < 0) {
        pthread_mutex_lock(&j->lock);
        if (j->errors++ < 10) fprintf(stderr, "DKIOCSYNCHRONIZECACHE failed: %s\n", strerror(errno));
        pthread_mutex_unlock(&j->lock);
    }
#endif
    return 0;
}

static void *worker(void *arg) {
    job_t *j = arg;
    uint8_t *buf, *exp = NULL;
    if (posix_memalign((void **)&buf, 16384, g_bs)) abort();
    uint64_t rs = (uint64_t)(uintptr_t)buf ^ hname(j->path) ^ (uint64_t)(now() * 1e9);
    if (!rs) rs = 7;
    if (j->mode == M_VERIFY || j->mode == M_CHECK) {
        if (posix_memalign((void **)&exp, 16384, STAMP)) abort();
    } else {
        for (uint64_t i = 0; i < g_bs / 8; i++) ((uint64_t *)buf)[i] = xs(&rs);
    }
    uint64_t hn = hname(j->name);
    uint64_t h[NBUCKET];
    memset(h, 0, sizeof h);
    uint64_t rops = 0, wops = 0, rb = 0, wb = 0, mx = 0;
    double t0 = now(), tend = t0 + g_dur;
    int phase = j->mode == M_CHECK;
    for (;;) {
        if (atomic_load(&j->stop)) break;
        uint64_t off;
        int wr;
        if (j->mode == M_VERIFY || j->mode == M_CHECK) {
            uint64_t i = atomic_fetch_add(&j->cursor, 1);
            if (phase == 0 && i >= j->nblk) {
                /* write phase done for this thread; last writer flips to read phase */
                if (atomic_fetch_sub(&j->writers_left, 1) == 1) {
                    pthread_mutex_lock(&j->lock);
                    j->wphase_secs = now() - t0;
                    pthread_mutex_unlock(&j->lock);
                    atomic_store(&j->cursor, 0);
                    __atomic_store_n(&j->phase, 1, __ATOMIC_RELEASE);
                }
                while (!__atomic_load_n(&j->phase, __ATOMIC_ACQUIRE)) usleep(1000);
                phase = 1;
                continue;
            }
            if (phase == 1 && i >= j->nblk) break;
            uint64_t k = g_rand ? (i * j->stride) % j->nblk : i;
            off = j->start + k * g_bs;
            wr = phase == 0;
            if (wr)
                for (uint64_t s = 0; s < g_bs; s += STAMP)
                    fill_stamp(buf + s, (off + s) / STAMP, g_seed, j->name, hn);
        } else {
            if (now() >= tend) break;
            if (j->bytes_cap && atomic_load(&j->live_bytes) >= j->bytes_cap) break;
            uint64_t k = g_rand ? xs(&rs) % j->nblk : atomic_fetch_add(&j->cursor, 1) % j->nblk;
            off = j->start + k * g_bs;
            wr = j->mode == M_WRITE || (j->mode == M_MIX && (int)(xs(&rs) % 100) >= g_readpct);
        }
        double a = now();
        int rc = do_io(j, wr, buf, off);
        uint64_t us = (uint64_t)((now() - a) * 1e6);
        if (rc) {
            if (j->errors > 100) { atomic_store(&j->stop, 1); break; }
            continue;
        }
        h[bucket(us)]++;
        if (us > mx) mx = us;
        if (wr) { wops++; wb += g_bs; } else { rops++; rb += g_bs; }
        atomic_fetch_add(&j->live_bytes, g_bs);
        atomic_fetch_add(&j->live_ops, 1);
        uint64_t cur = atomic_load(&j->live_max);
        while (us > cur && !atomic_compare_exchange_weak(&j->live_max, &cur, us)) {}
        if ((j->mode == M_VERIFY || j->mode == M_CHECK) && !wr) {
            for (uint64_t s = 0; s < g_bs; s += STAMP) {
                fill_stamp(exp, (off + s) / STAMP, g_seed, j->name, hn);
                if (memcmp(buf + s, exp, STAMP)) mismatch(j, off + s, buf + s, exp);
            }
        }
    }
    double el = now() - t0;
    pthread_mutex_lock(&j->lock);
    for (int i = 0; i < NBUCKET; i++) j->hist[i] += h[i];
    j->rd_ops += rops; j->wr_ops += wops; j->rd_bytes += rb; j->wr_bytes += wb;
    if (mx > j->maxlat) j->maxlat = mx;
    if (el > j->secs) j->secs = el;
    if ((j->mode == M_VERIFY || j->mode == M_CHECK) && phase == 1) {
        double r = el - j->wphase_secs;
        if (r > j->rphase_secs) j->rphase_secs = r;
    }
    pthread_mutex_unlock(&j->lock);
    free(buf); free(exp);
    return NULL;
}

static void *reporter(void *arg) {
    (void)arg;
    FILE *f = fopen(g_tcsv, "a");
    if (!f) return NULL;
    uint64_t prev[MAXJOBS] = {0}, prevops[MAXJOBS] = {0};
    double t0 = now(), last = t0;
    while (!atomic_load(&g_done)) {
        double target = last + g_ival;
        while (!atomic_load(&g_done) && now() < target) usleep(100000);
        double t = now(), dt = t - last;
        last = t;
        double tot = 0;
        for (int i = 0; i < g_njobs; i++) {
            job_t *j = &g_jobs[i];
            uint64_t b = atomic_load(&j->live_bytes), o = atomic_load(&j->live_ops);
            uint64_t m = atomic_exchange(&j->live_max, 0);
            double mbs = (b - prev[i]) / dt / 1e6;
            tot += mbs;
            fprintf(f, "%s,%.0f,%s,%s,%.1f,%.0f,%.1f\n", g_label, t - t0, j->path, j->zone, mbs,
                    (o - prevops[i]) / dt, m / 1000.0);
            prev[i] = b; prevops[i] = o;
        }
        fprintf(f, "%s,%.0f,ALL,,%.1f,,\n", g_label, t - t0, tot);
        fflush(f);
    }
    fclose(f);
    return NULL;
}

static const char *mname(int m) { return m == M_READ ? "read" : m == M_WRITE ? "write" : m == M_MIX ? "mix" : m == M_CHECK ? "check" : "verify"; }

int main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "b:q:p:t:B:M:s:F:o:l:T:i:n")) != -1) {
        switch (c) {
        case 'b': g_bs = parse_size(optarg); break;
        case 'q': g_qd = atoi(optarg); break;
        case 'p': g_rand = !strcmp(optarg, "rand"); break;
        case 't': g_dur = atof(optarg); break;
        case 'B': g_bytes = parse_size(optarg); break;
        case 'M': g_readpct = atoi(optarg); break;
        case 's': g_seed = strtoull(optarg, NULL, 0); break;
        case 'F': g_fsync = atoi(optarg); break;
        case 'o': g_csv = optarg; break;
        case 'l': g_label = optarg; break;
        case 'T': g_tcsv = optarg; break;
        case 'i': g_ival = atof(optarg); break;
        case 'n': g_dry = 1; break;
        default: fprintf(stderr, "see the header of sasbench.c for usage\n"); return 2;
        }
    }
    if (optind >= argc || g_bs < 512 || g_bs % 512 || g_qd < 1) {
        fprintf(stderr, "usage: sasbench [-b bs] [-q qd] [-p seq|rand] [-t s] [-B bytes] [-M pct] [-s seed] [-F 0|1|2] [-o csv] [-l label] [-T tcsv] [-i s] [-n] path[,r|w|m|v[,zone[,name]]] ...\n");
        return 2;
    }
    for (int a = optind; a < argc && g_njobs < MAXJOBS; a++) {
        job_t *j = &g_jobs[g_njobs++];
        char tmp[512], *sv = NULL;
        snprintf(tmp, sizeof tmp, "%s", argv[a]);
        char *p = strtok_r(tmp, ",", &sv), *m = strtok_r(NULL, ",", &sv),
             *z = strtok_r(NULL, ",", &sv), *n = strtok_r(NULL, ",", &sv);
        snprintf(j->path, sizeof j->path, "%s", p);
        j->mode = !m || *m == 'r' ? M_READ : *m == 'w' ? M_WRITE : *m == 'm' ? M_MIX : *m == 'c' ? M_CHECK : M_VERIFY;
        snprintf(j->zone, sizeof j->zone, "%s", z ? z : "outer");
        snprintf(j->name, sizeof j->name, "%s", n ? n : p);
        if ((j->mode == M_VERIFY || j->mode == M_CHECK) && g_bs % STAMP) { fprintf(stderr, "verify needs bs %% 4096 == 0\n"); return 2; }
        j->fd = open(j->path, j->mode == M_READ || j->mode == M_CHECK ? O_RDONLY : O_RDWR);
        if (j->fd < 0) { perror(j->path); return 1; }
#ifdef F_NOCACHE
        fcntl(j->fd, F_NOCACHE, 1);
#endif
        uint64_t size = 0;
#ifdef DKIOCGETBLOCKCOUNT
        uint64_t cnt = 0; uint32_t bsz = 0;
        if (!ioctl(j->fd, DKIOCGETBLOCKCOUNT, &cnt) && !ioctl(j->fd, DKIOCGETBLOCKSIZE, &bsz)) size = cnt * bsz;
#endif
        if (!size) { struct stat st; if (!fstat(j->fd, &st)) size = (uint64_t)st.st_size; }
        if (!size) size = (uint64_t)lseek(j->fd, 0, SEEK_END);
        double lo, hi;
        if (!strcmp(j->zone, "outer")) lo = 0, hi = 2;
        else if (!strcmp(j->zone, "middle")) lo = 49, hi = 51;
        else if (!strcmp(j->zone, "inner")) lo = 98, hi = 100;
        else if (!strcmp(j->zone, "full")) lo = 0, hi = 100;
        else if (sscanf(j->zone, "%lf-%lf", &lo, &hi) != 2 || lo < 0 || hi > 100 || hi <= lo) {
            fprintf(stderr, "bad zone %s\n", j->zone); return 2;
        }
        uint64_t s = (uint64_t)(size * (lo / 100.0)), e = (uint64_t)(size * (hi / 100.0));
        s = (s + g_bs - 1) / g_bs * g_bs;
        e = e / g_bs * g_bs;
        if (e > size / g_bs * g_bs) e = size / g_bs * g_bs;
        j->start = s;
        j->nblk = e > s ? (e - s) / g_bs : 0;
        if ((j->mode == M_VERIFY || j->mode == M_CHECK) && g_bytes && g_bytes / g_bs < j->nblk) j->nblk = g_bytes / g_bs;
        if (!j->nblk) { fprintf(stderr, "%s: empty window\n", j->path); return 2; }
        j->bytes_cap = j->mode == M_VERIFY || j->mode == M_CHECK ? 0 : g_bytes;
        j->stride = 1;
        if (g_rand && (j->mode == M_VERIFY || j->mode == M_CHECK)) {
            uint64_t st = (uint64_t)(j->nblk * 0.618034) | 1;
            while (gcd(st, j->nblk) != 1) st += 2;
            j->stride = st;
        }
        atomic_store(&j->writers_left, g_qd);
        pthread_mutex_init(&j->lock, NULL);
        printf("job %d: %s %s zone=%s name=%s bytes [%" PRIu64 ", %" PRIu64 ") lba512 [%" PRIu64
               ", %" PRIu64 ") blocks=%" PRIu64 " disk=%" PRIu64 "\n",
               g_njobs - 1, j->path, mname(j->mode), j->zone, j->name, j->start, j->start + j->nblk * g_bs,
               j->start / 512, (j->start + j->nblk * g_bs) / 512, j->nblk, size);
    }
    fflush(stdout);
    if (g_dry) return 0;
    pthread_t th[MAXJOBS * 64], rep;
    int nt = 0;
    if (g_tcsv) pthread_create(&rep, NULL, reporter, NULL);
    double t0 = now();
    for (int i = 0; i < g_njobs; i++)
        for (int q = 0; q < g_qd && nt < MAXJOBS * 64; q++) pthread_create(&th[nt++], NULL, worker, &g_jobs[i]);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    double wall = now() - t0;
    atomic_store(&g_done, 1);
    if (g_tcsv) pthread_join(rep, NULL);

    FILE *csv = g_csv ? fopen(g_csv, "a") : NULL;
    if (csv && ftell(csv) == 0)
        fprintf(csv, "label,job,path,name,mode,pattern,bs,qd,zone,secs,read_MBps,write_MBps,MBps,IOPS,p50_us,p99_us,max_us,errors,mismatches,verify_w_MBps,verify_r_MBps\n");
    uint64_t agg[NBUCKET] = {0}, ab = 0, ao = 0, amx = 0, aerr = 0, amis = 0;
    double arb = 0, awb = 0;
    for (int i = 0; i < g_njobs; i++) {
        job_t *j = &g_jobs[i];
        uint64_t ops = j->rd_ops + j->wr_ops, by = j->rd_bytes + j->wr_bytes;
        double rmb = j->rd_bytes / j->secs / 1e6, wmb = j->wr_bytes / j->secs / 1e6;
        double vw = 0, vr = 0;
        if (j->mode == M_VERIFY || j->mode == M_CHECK) {
            vw = j->wphase_secs > 0 ? j->wr_bytes / j->wphase_secs / 1e6 : 0;
            vr = j->rphase_secs > 0 ? j->rd_bytes / j->rphase_secs / 1e6 : 0;
        }
        for (int k = 0; k < NBUCKET; k++) agg[k] += j->hist[k];
        ab += by; ao += ops; aerr += j->errors; amis += j->mismatches;
        arb += j->rd_bytes; awb += j->wr_bytes;
        if (j->maxlat > amx) amx = j->maxlat;
        printf("%-14s %-6s %-8s %-5s bs=%-8" PRIu64 " qd=%-2d %6.1fs  R %7.1f MB/s  W %7.1f MB/s  %8.0f IOPS  p50 %7" PRIu64 "us p99 %8" PRIu64 "us max %8" PRIu64 "us err %" PRIu64 " mis %" PRIu64,
               j->path, mname(j->mode), j->zone, g_rand ? "rand" : "seq", g_bs, g_qd, j->secs, rmb, wmb,
               ops / j->secs, pct(j->hist, .5), pct(j->hist, .99), j->maxlat, j->errors, j->mismatches);
        if (j->mode == M_VERIFY || j->mode == M_CHECK) printf("  (verify W %.1f R %.1f MB/s, %" PRIu64 " blocks of 4K checked)", vw, vr, j->rd_bytes / STAMP);
        printf("\n");
        if (csv)
            fprintf(csv, "%s,%d,%s,%s,%s,%s,%" PRIu64 ",%d,%s,%.1f,%.1f,%.1f,%.1f,%.0f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.1f,%.1f\n",
                    g_label, i, j->path, j->name, mname(j->mode), g_rand ? "rand" : "seq", g_bs, g_qd, j->zone,
                    j->secs, rmb, wmb, rmb + wmb, ops / j->secs, pct(j->hist, .5), pct(j->hist, .99), j->maxlat,
                    j->errors, j->mismatches, vw, vr);
    }
    printf("AGGREGATE %d jobs %.1fs  R %.1f MB/s  W %.1f MB/s  total %.1f MB/s  %.0f IOPS  p50 %" PRIu64 "us p99 %" PRIu64 "us max %" PRIu64 "us errors %" PRIu64 " mismatches %" PRIu64 "\n",
           g_njobs, wall, arb / wall / 1e6, awb / wall / 1e6, ab / wall / 1e6, ao / wall, pct(agg, .5), pct(agg, .99), amx, aerr, amis);
    if (csv) {
        fprintf(csv, "%s,ALL,,,,%s,%" PRIu64 ",%d,,%.1f,%.1f,%.1f,%.1f,%.0f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",,\n",
                g_label, g_rand ? "rand" : "seq", g_bs, g_qd, wall, arb / wall / 1e6, awb / wall / 1e6, ab / wall / 1e6,
                ao / wall, pct(agg, .5), pct(agg, .99), amx, aerr, amis);
        fclose(csv);
    }
    return amis ? 3 : aerr ? 1 : 0;
}
