/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 *
 * mps_xnu_os.cpp - the FreeBSD-shaped core on xnu.
 *
 * 1. The mps_os_*() platform hooks of core/compat/mps_compat.h and
 *    core/mps_xnu.h (test/mps_host_os.cpp is the host counterpart), with
 *    public KPIs only:
 *      locks       IOLock (lck_mtx); the owner is tracked by compat (mtx_owner)
 *      sleep       IOLockSleep/IOLockSleepDeadline, IOLockWakeup, IODelay/IOSleep
 *      clocks      mach_absolute_time, the calendar clock
 *      memory      IOMalloc/IOFree
 *      registers   the BAR1 mapping (mps_bhandle = its virtual address)
 *      DMA         IOBufferMemoryDescriptor + IODMACommand on the device's
 *                  IOMapper (DART): IOVA < 4 GiB for a 32-bit lowaddr; several
 *                  segments allowed when the tag allows them (chain pool)
 *      callouts    one thread_call per mutex, armed for its earliest callout
 *      taskqueue   one kernel thread per queue (FreeBSD's model; the event and reinit tasks)
 *      log         os_log, one category per mps_dprint class, or IOLog
 * 2. The mps_kext_*() calls of mps_xnu_os.h for the IOKit class: the call
 *    sequence of test/mock_ioc_test.cpp `Rig', ccbs for SCSI IO and task
 *    management, completions queued for the class to deliver without
 *    mps_mtx (IOSCSIParallelFamily must not be called with it held).
 *
 * This is the only kext translation unit that includes the core headers:
 * core/compat/queue.h and <sys/queue.h> (pulled in by IOService.h) cannot
 * meet. None of the headers below includes <sys/queue.h>.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <libkern/OSAtomic.h>
#include <os/log.h>

/* IOLib.h's deprecated min()/max() macros vs compat's inline functions */
#undef min
#undef max

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#define MPS_COMPAT_NO_LIBC_REMAP
extern "C" {
#include "compat/mps_compat.h"
#include "compat/cam_compat.h"
#include "mps_mpi.h"
#include "mpi/mpi2_init.h"
#include "mpsvar.h"
#include "mps_sas.h"
#include "mps_table.h"
#include "mps_xnu.h"
}
#pragma clang diagnostic pop

#include "mps_xnu_os.h"

static_assert(MPSK_INFO == MPS_INFO && MPSK_FAULT == MPS_FAULT && MPSK_EVENT == MPS_EVENT &&
              MPSK_RECOVERY == MPS_RECOVERY && MPSK_ERROR == MPS_ERROR && MPSK_INIT == MPS_INIT &&
              MPSK_XINFO == MPS_XINFO && MPSK_USER == MPS_USER && MPSK_MAPPING == MPS_MAPPING &&
              MPSK_DEBUG_ALL == MPS_DEBUG_ALL, "mps_dprint classes");
static_assert(MPSK_DEVINFO_SSP_TARGET == MPI2_SAS_DEVICE_INFO_SSP_TARGET &&
              MPSK_DEVINFO_STP_TARGET == MPI2_SAS_DEVICE_INFO_STP_TARGET &&
              MPSK_DEVINFO_SATA_DEVICE == MPI2_SAS_DEVICE_INFO_SATA_DEVICE, "device info bits");
static_assert(MPSK_LEVEL_INFO == MPS_LEVEL_INFO && MPSK_LEVEL_TRACE == MPS_LEVEL_TRACE, "levels");
static_assert(sizeof(struct mps_kext_seg) == sizeof(bus_dma_segment_t) &&
              offsetof(struct mps_kext_seg, len) == offsetof(bus_dma_segment_t, ds_len), "segments");

/* ======================================================================
 * Logging: one os_log handle per FreeBSD debug class (= category).
 * ==================================================================== */

/* os_log subsystem = the bundle identifier (Makefile: SAS2308_BUNDLE_ID = CFBundleIdentifier) */
#ifndef SAS2308_BUNDLE_ID
#define SAS2308_BUNDLE_ID "org.sas2308.driver.SAS2308HBA"
#endif
static const char *const kLogSubsystem = SAS2308_BUNDLE_ID;
static const char *const kLogCats[] = {
    "info", "fault", "event", "log", "recovery", "error", "init", "xinfo", "user", "mapping", "trace",
};
static constexpr unsigned kNumLogCats = sizeof(kLogCats) / sizeof(kLogCats[0]);
static os_log_t gLogs[kNumLogCats];          /* created once per kext load, never released */
static volatile SInt32 gLogInit = 0;         /* 0 none, 1 creating, 2 ready */

static void CreateLogs()
{
    if (!OSCompareAndSwap(0, 1, &gLogInit))
        return;
    for (unsigned i = 0; i < kNumLogCats; i++)
        gLogs[i] = os_log_create(kLogSubsystem, kLogCats[i]);
    OSCompareAndSwap(1, 2, &gLogInit);
}

static os_log_t LogFor(uint32_t cls)
{
    if (gLogInit != 2)
        return OS_LOG_DEFAULT;
    const char *cat = mps_xnu_category(cls);
    for (unsigned i = 0; i < kNumLogCats; i++)
        if (strcmp(cat, kLogCats[i]) == 0)
            return gLogs[i] ? gLogs[i] : OS_LOG_DEFAULT;
    return OS_LOG_DEFAULT;
}

/* os_log type: mps_xnu_log_type (core/mps_xnu.c, host-tested) */
static os_log_type_t LogType(uint32_t cls)
{
    switch (mps_xnu_log_type(cls)) {
    case MPS_XNU_LOG_FAULT:
        return OS_LOG_TYPE_FAULT;
    case MPS_XNU_LOG_ERROR:
        return OS_LOG_TYPE_ERROR;
    case MPS_XNU_LOG_DEFAULT:
        return OS_LOG_TYPE_DEFAULT;
    default:
        return OS_LOG_TYPE_DEBUG;
    }
}

extern "C" void mps_kext_log(const struct mps_kext_host *host, uint32_t cls, const char *line)
{
    host = mps_kext_log_host(host);           /* device-less lines: the last host's sink and unit (D25) */
    const unsigned unit = host ? host->unit : 0;
    if (host && host->use_iolog) {
        IOLog("SAS2308HBA%u: [%s] %s\n", unit, mps_xnu_category(cls), line);
        return;
    }
    /*
     * Plain %s: the kernel os_log encoder (xnu libkern/os/log_encode.c
     * log_encode_fmt) has no privacy annotations; a "%" followed by "{public}" hits its
     * default case, returns EINVAL and the line is dropped (counted in
     * oslog_p_fmt_invalid_msgcount), and the msgbuf copy prints "{public}s".
     * Kernel string arguments are always encoded public.
     */
    os_log_with_type(LogFor(cls), LogType(cls), "hba%u: %s", unit, line);
}

static struct mps_kext_host *HostOf(device_t dev)
{
    return dev ? (struct mps_kext_host *)dev->dv_os : nullptr;
}

/* ======================================================================
 * mps_os_* hooks
 * ==================================================================== */

/* A compat lock: the IOLock plus, for a struct mtx, its callout thread call. */
struct XnuLock {
    IOLock         *lock;
    thread_call_t   callout;     /* mps_os_callout_arm(), allocated on first use */
    struct mtx     *mtx;
    volatile SInt32 inflight;    /* callout invocations queued or running */
};

/* One per taskqueue (tq_os): its kernel thread runs mps_compat_taskqueue_thread(). */
struct XnuTq {
    thread_t        thread;
};

struct XnuDma {
    IOBufferMemoryDescriptor *bmd;
    IODMACommand             *cmd;
    bus_dma_segment_t        *segs;
    uint32_t                  nsegs;
};

/*
 * IOLockWakeup() wants a lock argument it does not use (it wakes the event
 * channel); wakeup(9) has no mutex to give it. One lock per kext load.
 */
static IOLock *gWakeLock;

static void XnuCalloutFire(thread_call_param_t p0, thread_call_param_t)
{
    XnuLock *xl = (XnuLock *)p0;
    mps_compat_callout_run(xl->mtx);
    OSDecrementAtomic(&xl->inflight);        /* last use of xl */
}

/*
 * A taskqueue's thread. taskqueue_free() waits until the loop has left the
 * queue (tq_exited); after that the thread touches neither tq nor x and
 * only terminates itself.
 */
static void XnuTqThread(void *arg, wait_result_t)
{
    mps_compat_taskqueue_thread((struct taskqueue *)arg);
    (void)thread_terminate(current_thread());
}

/* Wait (outside every lock) until no invocation of a thread call is left. */
static void WaitIdle(volatile SInt32 *inflight)
{
    while (*inflight > 0)
        IOSleep(1);
}

extern "C" {

/*
 * malloc(9) for M_WAITOK and M_NOWAIT alike (D38): IOMalloc may block, which
 * is legal everywhere the core runs: no core path runs at primary interrupt
 * level or with preemption disabled (FilterInterruptRequest returns without
 * entering the core; the rest is work-loop, thread-call, taskqueue and client
 * threads). IOMalloc can return NULL even for M_WAITOK (IOMalloc_internal:
 * Z_WAITOK without Z_NOFAIL): every M_WAITOK caller checks it, FreeBSD's
 * mpssas_realloc_targets included since L13.
 */
void *mps_os_malloc(size_t size, int canwait)
{
    (void)canwait;
    return IOMalloc(size);
}

void mps_os_free(void *ptr, size_t size)
{
    IOFree(ptr, size);
}

void *mps_os_lock_alloc(const char *name)
{
    (void)name;
    XnuLock *xl = (XnuLock *)IOMalloc(sizeof(*xl));
    if (xl == nullptr)
        return nullptr;
    bzero(xl, sizeof(*xl));
    xl->lock = IOLockAlloc();
    if (xl->lock == nullptr) {
        IOFree(xl, sizeof(*xl));
        return nullptr;
    }
    return xl;
}

void mps_os_lock_free(void *lock)
{
    XnuLock *xl = (XnuLock *)lock;
    if (xl == nullptr)
        return;
    if (xl->callout != nullptr) {
        /* mtx_destroy(): no callout is armed any more; let a firing one finish */
        if (thread_call_cancel(xl->callout))
            OSDecrementAtomic(&xl->inflight);
        WaitIdle(&xl->inflight);
        thread_call_free(xl->callout);
    }
    IOLockFree(xl->lock);
    IOFree(xl, sizeof(*xl));
}

void mps_os_lock(void *lock)
{
    IOLockLock(((XnuLock *)lock)->lock);
}

void mps_os_unlock(void *lock)
{
    IOLockUnlock(((XnuLock *)lock)->lock);
}

void *mps_os_curthread(void)
{
    return (void *)current_thread();
}

int mps_os_sleep(void *lock, void *chan, uint64_t timo_us)
{
    IOLock *l = ((XnuLock *)lock)->lock;
    if (timo_us == 0) {
        (void)IOLockSleep(l, chan, THREAD_UNINT);
        return 0;
    }
    uint64_t deadline;
    if (timo_us <= 0xFFFFFFFFull)
        clock_interval_to_deadline((uint32_t)timo_us, kMicrosecondScale, &deadline);
    else
        clock_interval_to_deadline((uint32_t)(timo_us / 1000), kMillisecondScale, &deadline);
    int wr = IOLockSleepDeadline(l, chan, deadline, THREAD_UNINT);
    return wr == THREAD_TIMED_OUT ? EWOULDBLOCK : 0;
}

void mps_os_wakeup(void *chan)
{
    if (gWakeLock != nullptr)
        IOLockWakeup(gWakeLock, chan, false);
}

void mps_os_delay(uint64_t us)
{
    if (us >= 1000) {
        IOSleep((unsigned)(us / 1000));
        us %= 1000;
    }
    if (us)
        IODelay((unsigned)us);
}

uint64_t mps_os_uptime_us(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

void mps_os_walltime(uint64_t *sec, uint32_t *usec)
{
    clock_sec_t secs;
    clock_usec_t usecs;
    clock_get_calendar_microtime(&secs, &usecs);
    *sec = (uint64_t)secs;
    *usec = (uint32_t)usecs;
}

/*
 * MMIO is little-endian, so is arm64. No mapping reads as all-ones (gone).
 *
 * Review H3: ARMv8 does not order a store to Normal memory (a request
 * frame, the reply free queue, a cleared reply-post slot) before a later
 * store to Device memory (the descriptor post, REPLY_FREE/POST_HOST_INDEX),
 * nor a Device load before later Normal loads (the reply frame a posted
 * descriptor points at). FreeBSD mps relies on x86 ordering and an empty
 * bus_dmamap_sync; Linux writel/readl issue these barriers.
 */
uint32_t mps_os_bus_read_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t off)
{
    (void)t;
    if (h == 0)
        return 0xFFFFFFFF;
    const uint32_t v = *(volatile uint32_t *)(h + off);
#if defined(__arm64__) || defined(__aarch64__)
    __asm__ volatile("dmb oshld" ::: "memory");     /* later DMA-memory loads after this read */
#endif
    return v;
}

void mps_os_bus_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t off, uint32_t val)
{
    (void)t;
    if (h == 0)
        return;
#if defined(__arm64__) || defined(__aarch64__)
    __asm__ volatile("dmb oshst" ::: "memory");     /* earlier DMA-memory stores before the doorbell */
#endif
    *(volatile uint32_t *)(h + off) = val;
}

/*
 * D73: bus_dmamap_sync() is a no-op (PCIe DMA through the DART is coherent),
 * which is only right if the IODMACommand maps our memory itself. One that
 * bounces (a copy buffer for an address or alignment it cannot reach) would
 * copy only at prepare/complete, not at the core's syncs: refused. The
 * family's task buffers are safe either way: their prepare() and complete()
 * synchronize (SAS2308HBA::PortTaskMapBegin / PortTaskUnmap).
 */
static bool XnuBounced(IODMACommand *cmd)
{
    return cmd->getIOMemoryDescriptor() != cmd->getMemoryDescriptor();
}

/*
 * bus_dmamem_alloc(): wired kernel memory run through an IODMACommand bound
 * to the device's mapper, so the IOC sees IOVAs (on Apple silicon every PCI
 * device sits behind a DART). addrBits 32 makes the mapper place the IOVA
 * below 4 GiB, which the MPI2 32-bit reply-frame/sense address fields need.
 * Without a mapper the bus address is the physical one: the buffer is then
 * physically contiguous when the tag allows one segment, and below 4 GiB.
 * The segment list is computed once here; load() hands out a copy.
 */
int mps_os_dmamem_alloc(bus_dma_tag_t tag, void **vaddr, void **cookie)
{
    struct mps_kext_host *host = HostOf(tag->dt_dev);
    struct mps_softc *sc = (struct mps_softc *)tag->dt_dev->dv_softc;
    IOMapper *mapper = host ? (IOMapper *)host->mapper : nullptr;
    const size_t size = (size_t)tag->dt_maxsize;
    const bool low32 = tag->dt_lowaddr <= 0xFFFFFFFFull;
    const uint64_t align = tag->dt_alignment ? tag->dt_alignment : 1;
    int maxsegs = tag->dt_nsegments;

    if (size == 0 || size > (64u << 20) || align > 4096)
        return EINVAL;
    if (maxsegs <= 0 || maxsegs > 4096)
        maxsegs = 4096;

    mach_vm_address_t physMask = ~(mach_vm_address_t)4095;
    if (!mapper && low32)
        physMask &= 0xFFFFFFFFull;
    IOOptionBits opts = kIODirectionInOut;
    if (!mapper && maxsegs == 1)
        opts |= kIOMemoryPhysicallyContiguous;

    XnuDma *d = (XnuDma *)IOMalloc(sizeof(*d));
    if (d == nullptr)
        return ENOMEM;
    bzero(d, sizeof(*d));
    d->segs = (bus_dma_segment_t *)IOMalloc(sizeof(bus_dma_segment_t) * (size_t)maxsegs);
    d->bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, opts, size, physMask);
    UInt64 maxSegSize = tag->dt_maxsegsz >= size ? 0 : tag->dt_maxsegsz;
    if (d->segs)
        d->cmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, low32 ? 32 : 64, maxSegSize,
                                                 IODMACommand::kMapped, 0, (UInt32)align, mapper, NULL);
    IOReturn ret = (d->segs && d->bmd && d->cmd) ? d->cmd->setMemoryDescriptor(d->bmd, true) : kIOReturnNoMemory;
    if (ret == kIOReturnSuccess && XnuBounced(d->cmd))
        ret = kIOReturnDMAError;

    UInt64 offset = 0;
    bool ok = ret == kIOReturnSuccess;
    while (ok && offset < size) {
        IODMACommand::Segment64 seg[16];
        UInt32 n = 16;
        ret = d->cmd->gen64IOVMSegments(&offset, seg, &n);
        if (ret != kIOReturnSuccess || n == 0) {
            ok = false;
            break;
        }
        for (UInt32 i = 0; i < n && ok; i++) {
            /* alignment: the DMA specification's (D74) */
            if (d->nsegs >= (uint32_t)maxsegs ||
                seg[i].fIOVMAddr + seg[i].fLength - 1 > tag->dt_lowaddr ||
                seg[i].fLength > tag->dt_maxsegsz) {
                ok = false;
                break;
            }
            d->segs[d->nsegs].ds_addr = seg[i].fIOVMAddr;
            d->segs[d->nsegs].ds_len = seg[i].fLength;
            d->nsegs++;
        }
    }
    if (!ok || offset != size) {
        if (sc)
            mps_dprint(sc, MPS_ERROR | MPS_INIT, "DMA map failed: ret 0x%x, %u segments for %llu of %zu "
                       "bytes (max %d segments, align %llu, lowaddr 0x%llx)\n", ret, d->nsegs,
                       (unsigned long long)offset, size, maxsegs, (unsigned long long)align,
                       (unsigned long long)tag->dt_lowaddr);
        mps_os_dmamem_free(tag, nullptr, d);
        return ENOMEM;
    }
    /* not zeroed here: bus_dmamem_alloc does for BUS_DMA_ZERO, as FreeBSD (D74) */
    if (sc)
        mps_dprint(sc, MPS_XINFO, "DMA %zu bytes, align %llu, lowaddr 0x%llx: IOVA 0x%llx, %u segment%s\n",
                   size, (unsigned long long)align, (unsigned long long)tag->dt_lowaddr,
                   (unsigned long long)d->segs[0].ds_addr, d->nsegs, d->nsegs == 1 ? "" : "s");
    *vaddr = d->bmd->getBytesNoCopy();
    *cookie = d;
    return 0;
}

void mps_os_dmamem_free(bus_dma_tag_t tag, void *vaddr, void *cookie)
{
    (void)tag;
    (void)vaddr;
    XnuDma *d = (XnuDma *)cookie;
    if (d == nullptr)
        return;
    if (d->cmd) {
        d->cmd->clearMemoryDescriptor(true);    /* completes: unmaps the IOVA */
        d->cmd->release();
    }
    if (d->bmd)
        d->bmd->release();
    if (d->segs) {
        int maxsegs = tag->dt_nsegments;
        if (maxsegs <= 0 || maxsegs > 4096)
            maxsegs = 4096;
        IOFree(d->segs, sizeof(bus_dma_segment_t) * (size_t)maxsegs);
    }
    IOFree(d, sizeof(*d));
}

int mps_os_dmamem_load(bus_dma_tag_t tag, void *cookie, bus_dma_segment_t *segs, int maxsegs, int *nsegs)
{
    (void)tag;
    XnuDma *d = (XnuDma *)cookie;
    if ((int)d->nsegs > maxsegs)
        return EFBIG;
    memcpy(segs, d->segs, sizeof(*segs) * d->nsegs);
    *nsegs = (int)d->nsegs;
    return 0;
}

void mps_os_dmamem_unload(bus_dma_tag_t tag, void *cookie)
{
    (void)tag;
    (void)cookie;                            /* the IOVA stays mapped until free */
}

/*
 * D6: a malloc() block of the core (config page, SATA IDENTIFY data) mapped
 * for the IOC at bus_dmamap_load(), as busdma loads any kernel buffer: an
 * IOMemoryDescriptor on the kernel range, prepared (wired), and an
 * IODMACommand on the device's mapper (DART) for the IOVA. Compat releases
 * it at unload, at the next load of the map or when the block is freed.
 * Called with mps_mtx held; both may block (allowed under a lck_mtx).
 */
struct XnuBufMap {
    IOMemoryDescriptor *md;
    IODMACommand       *cmd;
};

void mps_os_dmamap_unload_buf(bus_dma_tag_t tag, void *cookie)
{
    (void)tag;
    XnuBufMap *b = (XnuBufMap *)cookie;
    if (b == nullptr)
        return;
    if (b->cmd) {
        b->cmd->clearMemoryDescriptor(true);    /* completes: unmaps the IOVA */
        b->cmd->release();
    }
    if (b->md) {
        b->md->complete();
        b->md->release();
    }
    IOFree(b, sizeof(*b));
}

int mps_os_dmamap_load_buf(bus_dma_tag_t tag, void *buf, size_t len, void **cookie,
                           bus_dma_segment_t *segs, int maxsegs, int *nsegs)
{
    struct mps_kext_host *host = HostOf(tag->dt_dev);
    struct mps_softc *sc = (struct mps_softc *)tag->dt_dev->dv_softc;
    IOMapper *mapper = host ? (IOMapper *)host->mapper : nullptr;
    const bool low32 = tag->dt_lowaddr <= 0xFFFFFFFFull;
    const uint64_t align = tag->dt_alignment ? tag->dt_alignment : 1;

    *cookie = nullptr;
    *nsegs = 0;
    XnuBufMap *b = (XnuBufMap *)IOMalloc(sizeof(*b));
    if (b == nullptr)
        return ENOMEM;
    bzero(b, sizeof(*b));
    b->md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)(uintptr_t)buf, len, kIODirectionInOut,
                                                 kernel_task);
    if (b->md == nullptr || b->md->prepare() != kIOReturnSuccess) {
        if (b->md)
            b->md->release();
        IOFree(b, sizeof(*b));
        return ENOMEM;
    }
    UInt64 maxSegSize = tag->dt_maxsegsz >= len ? 0 : tag->dt_maxsegsz;
    b->cmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, low32 ? 32 : 64, maxSegSize,
                                             IODMACommand::kMapped, 0, (UInt32)align, mapper, NULL);
    IOReturn ret = b->cmd ? b->cmd->setMemoryDescriptor(b->md, true) : kIOReturnNoMemory;
    UInt64 offset = 0;
    int n = 0, error = (ret == kIOReturnSuccess) ? 0 : ENOMEM;
    if (error == 0 && XnuBounced(b->cmd)) {
        ret = kIOReturnDMAError;
        error = EINVAL;
    }
    while (error == 0 && offset < len) {
        IODMACommand::Segment64 seg[4];
        UInt32 cnt = 4;
        ret = b->cmd->gen64IOVMSegments(&offset, seg, &cnt);
        if (ret != kIOReturnSuccess || cnt == 0) {
            error = ENOMEM;
            break;
        }
        for (UInt32 i = 0; i < cnt; i++) {
            if (n >= maxsegs) {
                error = EFBIG;
                break;
            }
            if (seg[i].fIOVMAddr + seg[i].fLength - 1 > tag->dt_lowaddr) {     /* alignment: the specification's (D74) */
                error = EINVAL;
                break;
            }
            segs[n].ds_addr = seg[i].fIOVMAddr;
            segs[n].ds_len = seg[i].fLength;
            n++;
        }
    }
    if (error != 0) {
        if (sc)
            mps_dprint(sc, MPS_ERROR, "DMA map of %zu bytes at %p failed: ret 0x%x, error %d, %d segments\n",
                       len, buf, ret, error, n);
        mps_os_dmamap_unload_buf(tag, b);
        return error;
    }
    *cookie = b;
    *nsegs = n;
    return 0;
}

/*
 * One thread call per mutex, armed for the earliest callout of that mutex;
 * it runs mps_compat_callout_run(m), which takes the mutex. `inflight'
 * counts invocations queued or running so mps_os_lock_free() can wait for
 * them (thread_call_cancel_wait is not a public KPI).
 */
void mps_os_callout_arm(struct mtx *m, uint64_t deadline_us)
{
    XnuLock *xl = (XnuLock *)m->mtx_os;
    if (deadline_us == 0) {
        if (xl->callout != nullptr && thread_call_cancel(xl->callout))
            OSDecrementAtomic(&xl->inflight);
        return;
    }
    if (xl->callout == nullptr) {
        xl->mtx = m;
        xl->callout = thread_call_allocate(XnuCalloutFire, xl);
        if (xl->callout == nullptr) {
            mps_kext_log(nullptr, MPS_FAULT, "cannot allocate the callout thread call: timers stopped");
            return;
        }
    }
    uint64_t abs;
    nanoseconds_to_absolutetime((deadline_us + 1) * 1000, &abs);   /* +1: never early after rounding */
    if (!thread_call_enter_delayed(xl->callout, abs))
        OSIncrementAtomic(&xl->inflight);
}

int mps_os_taskqueue_start(struct taskqueue *tq)
{
    XnuTq *x = (XnuTq *)IOMalloc(sizeof(*x));
    if (x == nullptr)
        return ENOMEM;
    bzero(x, sizeof(*x));
    tq->tq_os = x;
    tq->tq_threads = 1;
    if (kernel_thread_start(XnuTqThread, tq, &x->thread) != KERN_SUCCESS) {
        tq->tq_threads = 0;
        tq->tq_os = nullptr;
        IOFree(x, sizeof(*x));
        return ENOMEM;
    }
    return 0;
}

/* a task was queued: wake the queue's thread (it sleeps on tq) */
void mps_os_taskqueue_kick(struct taskqueue *tq)
{
    mps_os_wakeup(tq);
}

/* taskqueue_drain() of a queue without a thread: not on xnu (start fails instead) */
void mps_os_taskqueue_wait(struct taskqueue *tq)
{
    (void)tq;
    IOSleep(1);
}

/* taskqueue_free(): the thread has left the queue (tq_exited) */
void mps_os_taskqueue_stop(struct taskqueue *tq)
{
    XnuTq *x = (XnuTq *)tq->tq_os;
    if (x == nullptr)
        return;
    thread_deallocate(x->thread);
    tq->tq_os = nullptr;
    IOFree(x, sizeof(*x));
}

void mps_os_log(device_t dev, uint32_t cls, const char *line)
{
    mps_kext_log(HostOf(dev), cls, line);
}

/* KASSERT / panic(): never panic the Mac for the driver's sake. */
void mps_os_assert_fail(const char *msg)
{
    char line[256];
    snprintf(line, sizeof(line), "KASSERT failed: %s", msg);
    mps_kext_log(nullptr, MPS_FAULT, line);
}

int mps_os_setup_interrupts(device_t dev)
{
    struct mps_kext_host *h = HostOf(dev);
    return (h && h->setup_interrupts) ? h->setup_interrupts(h->hba) : 0;
}

void mps_os_free_interrupts(device_t dev)
{
    struct mps_kext_host *h = HostOf(dev);
    if (h && h->free_interrupts)
        h->free_interrupts(h->hba);
}

int mps_os_pci_restore(device_t dev)
{
    struct mps_kext_host *h = HostOf(dev);
    return (h && h->pci_restore) ? h->pci_restore(h->hba) : 0;
}

void mps_os_controller_dead(device_t dev)
{
    struct mps_kext_host *h = HostOf(dev);
    if (h && h->controller_dead)
        h->controller_dead(h->hba);
}

int mps_os_getenv(device_t dev, const char *name, char *buf, size_t len)
{
    struct mps_kext_host *h = HostOf(dev);
    return (h && h->getenv) ? h->getenv(h->hba, name, buf, len) : 0;
}

int mps_os_pci_present(device_t dev)
{
    struct mps_kext_host *h = HostOf(dev);
    return (h && h->pci_present) ? h->pci_present(h->hba) : 1;
}

} /* extern "C" */

/* ======================================================================
 * mps_kext_os_init: once per kext load (logs, the wakeup lock), for
 * mps_kext_create (kext/mps_kext.cpp)
 * ==================================================================== */

extern "C" int mps_kext_os_init(void)
{
    CreateLogs();
    if (gWakeLock == nullptr) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gWakeLock))
            IOLockFree(l);
    }
    return gWakeLock != nullptr ? 0 : -1;
}
