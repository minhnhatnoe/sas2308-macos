/*
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
 *      taskqueue   one thread_call per queue (the reinit task)
 *      log         os_log, one category per mps_dprint class (M4), or IOLog
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
static_assert(MPSK_MAX_TARGETS == MPS_XNU_MAX_TARGETS && MPSK_MAX_TASKS == MPS_XNU_IO_CMDS, "sizes");
static_assert(sizeof(struct mps_kext_seg) == sizeof(bus_dma_segment_t) &&
              offsetof(struct mps_kext_seg, len) == offsetof(bus_dma_segment_t, ds_len), "segments");

/* ======================================================================
 * Logging: one os_log handle per FreeBSD debug class (= category), M4.
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

/*
 * os_log type: errors/faults are ERROR; lines tagged with an info-level
 * class are DEFAULT (shown by `log stream`, persisted); XINFO/TRACE/LOG/USER
 * only lines are DEBUG (need `log stream --level debug`).
 */
static os_log_type_t LogType(uint32_t cls)
{
    if (cls & (MPS_FAULT | MPS_ERROR))
        return OS_LOG_TYPE_ERROR;
    if (cls & (MPS_INFO | MPS_RECOVERY | MPS_INIT | MPS_EVENT | MPS_MAPPING))
        return OS_LOG_TYPE_DEFAULT;
    return OS_LOG_TYPE_DEBUG;
}

extern "C" void mps_kext_log(const struct mps_kext_host *host, uint32_t cls, const char *line)
{
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

/* One per taskqueue (tq_os): runs mps_compat_taskqueue_run() on a thread call. */
struct XnuTq {
    thread_call_t   call;
    volatile SInt32 inflight;    /* invocations queued or running */
    volatile int    stopped;
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

static void XnuTqRun(thread_call_param_t p0, thread_call_param_t)
{
    struct taskqueue *tq = (struct taskqueue *)p0;
    XnuTq *x = (XnuTq *)tq->tq_os;
    mps_compat_taskqueue_run(tq);
    OSDecrementAtomic(&x->inflight);         /* last use of tq and x */
}

/* Wait (outside every lock) until no invocation of a thread call is left. */
static void WaitIdle(volatile SInt32 *inflight)
{
    while (*inflight > 0)
        IOSleep(1);
}

extern "C" {

void *mps_os_malloc(size_t size, int canwait)
{
    (void)canwait;                           /* mps_mtx is a blocking mutex: IOMalloc may block */
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

/* MMIO is little-endian, so is arm64. No mapping reads as all-ones (gone). */
uint32_t mps_os_bus_read_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t off)
{
    (void)t;
    if (h == 0)
        return 0xFFFFFFFF;
    return *(volatile uint32_t *)(h + off);
}

void mps_os_bus_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t off, uint32_t val)
{
    (void)t;
    if (h != 0)
        *(volatile uint32_t *)(h + off) = val;
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
                                                 IODMACommand::kMapped, 0, 1, mapper, NULL);
    IOReturn ret = (d->segs && d->bmd && d->cmd) ? d->cmd->setMemoryDescriptor(d->bmd, true) : kIOReturnNoMemory;

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
            if (d->nsegs >= (uint32_t)maxsegs || seg[i].fIOVMAddr % align != 0 ||
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
    bzero(d->bmd->getBytesNoCopy(), size);
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
    x->call = thread_call_allocate(XnuTqRun, tq);
    if (x->call == nullptr) {
        IOFree(x, sizeof(*x));
        return ENOMEM;
    }
    tq->tq_os = x;
    return 0;
}

void mps_os_taskqueue_kick(struct taskqueue *tq)
{
    XnuTq *x = (XnuTq *)tq->tq_os;
    if (x == nullptr || x->stopped)
        return;
    if (!thread_call_enter(x->call))
        OSIncrementAtomic(&x->inflight);
}

/* taskqueue_drain(): the queue runs on its thread call; give it time. */
void mps_os_taskqueue_wait(struct taskqueue *tq)
{
    mps_os_taskqueue_kick(tq);
    IOSleep(1);
}

/* taskqueue_free(): no more runs; wait for a running one (compat runs the rest inline). */
void mps_os_taskqueue_stop(struct taskqueue *tq)
{
    XnuTq *x = (XnuTq *)tq->tq_os;
    if (x == nullptr)
        return;
    x->stopped = 1;
    if (thread_call_cancel(x->call))
        OSDecrementAtomic(&x->inflight);
    WaitIdle(&x->inflight);
    thread_call_free(x->call);
    tq->tq_os = nullptr;
    IOFree(x, sizeof(*x));
}

void mps_os_log(device_t dev, uint32_t cls, const char *line)
{
    mps_kext_log(HostOf(dev), cls, line);
}

/* KASSERT / panic(): never panic the Mac for the driver's sake (M2-M4). */
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

} /* extern "C" */

/* ======================================================================
 * mps_kext_*: the IOKit class's view of the core
 * ==================================================================== */

#define MPSK_TASK_MAXSEGS	(MPS_XNU_MAXIO_PAGES + 1)	/* buffer_dmat: maxio + 1 */
#define MPSK_SEG_BYTES		0xFFFFFFull			/* MPI2 simple SGE length */

/* The HBA data of one SCSIParallelTask. */
struct mps_kext_task {
    union ccb               ccb;
    struct cam_path         path;
    struct mps_kext        *k;
    void                   *os_task;
    uint64_t                os_tag;     /* the family's tag: AbortTaskRequest */
    struct mps_kext_task   *done_next;
    int                     state;
#define MPSK_T_IDLE         0
#define MPSK_T_SUBMIT       1   /* inside xpt_action() */
#define MPSK_T_POSTED       2   /* the core (or the thin CAM) has it */
#define MPSK_T_SYNC_DONE    3   /* completed inside xpt_action() */
#define MPSK_T_QUEUED       4   /* on the done queue */
    bus_dma_segment_t       segs[MPSK_TASK_MAXSEGS];
};

/* An OS task-management request (XPT_XNU_TM). */
struct mps_kext_tmreq {
    union ccb               ccb;
    struct cam_path         path;
    struct mps_kext        *k;
    volatile int            done;
    int                     orphan;     /* the waiter left (stop): free on completion */
};

struct mps_kext {
    struct mps_softc        sc;
    struct device           dev;
    struct mps_kext_host   *host;
    int                     attached;   /* mps_attach succeeded, mps_free not yet */
    int                     detached;   /* mps_free ran: stats readable without a lock */
    int                     leaked;     /* mps_free failed: keep the memory */
    int                     stopping;   /* mps_kext_stop_waiters() */
    int                     tm_waiters;
    int                     tm_chan;
    void                   *inline_owner;   /* this thread drains the done queue itself */
    IOLock                 *done_lock;      /* leaf lock for the done queue */
    struct mps_kext_task   *done_head, *done_tail;
};

static volatile SInt32 gKexts = 0;

static void mps_kext_result(struct mps_kext_task *t, struct mps_kext_io_result *res)
{
    struct ccb_scsiio *csio = &t->ccb.csio;
    const uint32_t st = t->ccb.ccb_h.status & CAM_STATUS_MASK;

    bzero(res, sizeof(*res));
    res->cam_status = st;
    switch (st) {
    case CAM_REQ_CMP:
    case CAM_SCSI_STATUS_ERROR:
        res->kind = MPSK_IO_STATUS;
        res->scsi_status = csio->scsi_status;
        res->transferred = csio->resid < csio->dxfer_len ? csio->dxfer_len - csio->resid : 0;
        if ((t->ccb.ccb_h.status & CAM_AUTOSNS_VALID) && csio->sense_len > csio->sense_resid) {
            res->sense = (const uint8_t *)&csio->sense_data;
            res->sense_len = (uint32_t)(csio->sense_len - csio->sense_resid);
        }
        break;
    case CAM_DEV_NOT_THERE:
    case CAM_SEL_TIMEOUT:
    case CAM_TID_INVALID:
    case CAM_PATH_INVALID:
    case CAM_NO_HBA:
        res->kind = MPSK_IO_GONE;
        break;
    case CAM_CMD_TIMEOUT:
        res->kind = MPSK_IO_TIMEOUT;
        break;
    case CAM_REQ_INVALID:
    case CAM_LUN_INVALID:
    case CAM_REQ_TOO_BIG:
    case CAM_FUNC_NOTAVAIL:
        res->kind = MPSK_IO_REJECT;
        break;
    default:            /* CAM_SCSI_BUS_RESET (reinit), CAM_REQ_ABORTED (OS TM), ... */
        res->kind = MPSK_IO_RETRY;
        break;
    }
}

/* ccb_h.cbfcnp of a SCSI IO: mps_mtx held, so only queue it. */
static void mps_kext_io_done(struct cam_periph *periph, union ccb *ccb)
{
    (void)periph;
    struct mps_kext_task *t = (struct mps_kext_task *)ccb->ccb_h.ppriv_ptr0;
    struct mps_kext *k = t->k;

    if (t->state == MPSK_T_SUBMIT) {
        t->state = MPSK_T_SYNC_DONE;            /* mps_kext_submit() answers for it */
        return;
    }
    t->state = MPSK_T_QUEUED;
    t->done_next = nullptr;
    IOLockLock(k->done_lock);
    if (k->done_tail)
        k->done_tail->done_next = t;
    else
        k->done_head = t;
    k->done_tail = t;
    IOLockUnlock(k->done_lock);
    /* a callout, the reinit task or another thread: the class delivers from a thread call */
    if (k->inline_owner != mps_os_curthread() && k->host->io_done_kick)
        k->host->io_done_kick(k->host->hba);
}

static void mps_kext_tm_done(struct cam_periph *periph, union ccb *ccb)
{
    (void)periph;
    struct mps_kext_tmreq *w = (struct mps_kext_tmreq *)ccb->ccb_h.ppriv_ptr0;
    struct mps_kext *k = w->k;

    if (w->orphan) {
        IOFree(w, sizeof(*w));
        return;
    }
    w->done = 1;
    wakeup(&k->tm_chan);
}

/* CAM async callback (mps_mtx held): AC_XNU_SCAN, AC_BUS_RESET, AC_SENT_BDR, AC_LOST_DEVICE. */
static void mps_kext_async(void *arg, uint32_t code, struct cam_path *path, void *a)
{
    struct mps_kext *k = (struct mps_kext *)arg;
    const target_id_t tid = path ? path->target_id : CAM_TARGET_WILDCARD;
    const uint64_t one = (tid < MPSK_MAX_TARGETS) ? (1ull << tid) : 0;
    uint64_t tids = 0, present = 0;
    int event;

    switch (code) {
    case AC_XNU_SCAN: {
        union ccb *scan = (union ccb *)a;
        event = MPSK_ASYNC_SCAN;
        tids = (tid == CAM_TARGET_WILDCARD || (scan && scan->ccb_h.func_code == XPT_SCAN_BUS)) ? ~0ull : one;
        break;
    }
    case AC_LOST_DEVICE:
        event = MPSK_ASYNC_LOST_DEVICE;
        tids = tid == CAM_TARGET_WILDCARD ? ~0ull : one;
        break;
    case AC_BUS_RESET:
        event = MPSK_ASYNC_BUS_RESET;
        break;
    case AC_SENT_BDR:
        event = MPSK_ASYNC_SENT_BDR;
        tids = one;
        break;
    default:
        return;
    }
    for (u_int i = 0; i < MPSK_MAX_TARGETS; i++)
        if ((tids & (1ull << i)) && mps_xnu_target_present(&k->sc, i, NULL))
            present |= 1ull << i;
    if (k->host->async)
        k->host->async(k->host->hba, event, tids, present);
}

extern "C" {

struct mps_kext *mps_kext_create(struct mps_kext_host *host, uint16_t devid)
{
    CreateLogs();
    if (gWakeLock == nullptr) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gWakeLock))
            IOLockFree(l);
        if (gWakeLock == nullptr)
            return nullptr;
    }
    struct mps_kext *k = (struct mps_kext *)IOMalloc(sizeof(*k));
    if (k == nullptr)
        return nullptr;
    bzero(k, sizeof(*k));
    k->done_lock = IOLockAlloc();
    if (k->done_lock == nullptr) {
        IOFree(k, sizeof(*k));
        return nullptr;
    }
    k->host = host;
    mps_compat_device_init(&k->dev, "mps", (int)host->unit, &k->sc, host, devid);
    k->sc.mps_dev = &k->dev;
    k->sc.mps_btag = 0;
    k->sc.mps_bhandle = (bus_space_handle_t)(uintptr_t)host->regs;
    OSIncrementAtomic(&gKexts);
    return k;
}

int mps_kext_attach(struct mps_kext *k, uint32_t debug_mask)
{
    struct mps_softc *sc = &k->sc;
    struct cam_path *path;
    int error;

    mps_get_tunables(sc);
    sc->mps_debug = debug_mask & MPS_DEBUG_ALL;
    if (bus_dma_tag_create(bus_get_dma_tag(&k->dev), 1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
                           NULL, NULL, BUS_SPACE_MAXSIZE_32BIT, BUS_SPACE_UNRESTRICTED,
                           BUS_SPACE_MAXSIZE_32BIT, 0, NULL, NULL, &sc->mps_parent_dmat) != 0)
        return ENOMEM;
    /* mps_attach() frees everything itself on failure: never mps_free() after it */
    error = mps_attach(sc);
    if (error != 0) {
        bus_dma_tag_destroy(sc->mps_parent_dmat);
        sc->mps_parent_dmat = NULL;
        return error;
    }
    k->attached = 1;
    if (xpt_create_path(&path, NULL, cam_sim_path(sc->sassc->sim), CAM_TARGET_WILDCARD,
                        CAM_LUN_WILDCARD) == CAM_REQ_CMP) {
        if (xpt_register_async(AC_XNU_SCAN | AC_BUS_RESET | AC_SENT_BDR | AC_LOST_DEVICE,
                               mps_kext_async, k, path) != CAM_REQ_CMP)
            mps_dprint(sc, MPS_FAULT | MPS_INIT, "cannot register the async callback: no hotplug\n");
        xpt_free_path(path);
    }
    return 0;
}

void mps_kext_start(struct mps_kext *k)
{
    (void)k;
    mps_compat_intrhook_run();               /* mps_startup: unmask, port enable */
}

void mps_kext_stop_waiters(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    if (!k->attached)
        return;
    mps_lock(sc);
    k->stopping = 1;
    wakeup(&k->tm_chan);
    while (k->tm_waiters > 0)
        msleep(&k->tm_waiters, &sc->mps_mtx, 0, "mpsstop", 10);
    mps_unlock(sc);
}

void mps_kext_detach(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    int error;

    if (!k->attached)
        return;
    k->attached = 0;
    error = mps_free(sc);
    if (error != 0) {
        /* the IOC may still own DMA memory: keep everything */
        mps_kext_log(k->host, MPS_FAULT, "mps_free failed: controller memory left allocated");
        k->leaked = 1;
        return;
    }
    bus_dma_tag_destroy(sc->mps_parent_dmat);
    sc->mps_parent_dmat = NULL;
    k->detached = 1;
}

void mps_kext_destroy(struct mps_kext *k)
{
    if (k == nullptr)
        return;
    if (k->attached || k->leaked || k->done_head != nullptr)
        return;                              /* still in use: leak rather than free */
    IOLockFree(k->done_lock);
    IOFree(k, sizeof(*k));
    OSDecrementAtomic(&gKexts);
}

uint32_t mps_kext_max_segs(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    if (!k->attached || sc->buffer_dmat == NULL || sc->buffer_dmat->dt_nsegments <= 0)
        return 0;
    return MIN((uint32_t)sc->buffer_dmat->dt_nsegments, (uint32_t)MPSK_TASK_MAXSEGS);
}

uint64_t mps_kext_max_seg_bytes(void)
{
    return MPSK_SEG_BYTES;
}

/* mps_intr_msi(); completions found here are delivered by the caller. */
void mps_kext_intr(struct mps_kext *k)
{
    if (!k->attached)
        return;
    /*
     * Set without mps_mtx: a stale value only costs an extra io_done_kick,
     * and only this thread ever stores this thread's id.
     */
    k->inline_owner = mps_os_curthread();
    mps_intr_msi(&k->sc);
    k->inline_owner = nullptr;
}

size_t mps_kext_task_size(void)
{
    return sizeof(struct mps_kext_task);
}

uint32_t mps_kext_task_maxsegs(void)
{
    return MPSK_TASK_MAXSEGS;
}

struct mps_kext_seg *mps_kext_task_segs(void *td)
{
    return (struct mps_kext_seg *)((struct mps_kext_task *)td)->segs;
}

/* The XPT_SCSI_IO ccb of mock_ioc_test.cpp Rig::NewIo/Go. */
int mps_kext_submit(struct mps_kext *k, void *td, void *os_task, uint64_t os_tag, uint32_t tid,
                    uint64_t lun, const uint8_t *cdb, uint8_t cdblen, int dir, uint32_t nsegs, uint64_t len,
                    struct mps_kext_io_result *res)
{
    struct mps_softc *sc = &k->sc;
    struct mps_kext_task *t = (struct mps_kext_task *)td;
    union ccb *ccb = &t->ccb;
    int rc;

    if (!k->attached || cdblen == 0 || cdblen > IOCDBLEN || nsegs > MPSK_TASK_MAXSEGS ||
        len > 0xFFFFFFFFull) {
        bzero(res, sizeof(*res));
        res->kind = k->attached ? MPSK_IO_REJECT : MPSK_IO_GONE;
        return MPSK_SUBMIT_DONE;
    }
    bzero(ccb, sizeof(*ccb));
    t->k = k;
    t->os_task = os_task;
    t->os_tag = os_tag;
    t->done_next = nullptr;
    t->path.path_id = cam_sim_path(sc->sassc->sim);
    t->path.target_id = tid;
    t->path.lun_id = lun;
    xpt_setup_ccb(&ccb->ccb_h, &t->path, CAM_PRIORITY_NORMAL);
    ccb->ccb_h.func_code = XPT_SCSI_IO;
    ccb->ccb_h.cbfcnp = mps_kext_io_done;
    ccb->ccb_h.ppriv_ptr0 = t;
    ccb->ccb_h.timeout = 0;                  /* the family times the task (HandleTimeout) */
    ccb->ccb_h.flags = (dir == MPSK_DIR_IN ? CAM_DIR_IN : dir == MPSK_DIR_OUT ? CAM_DIR_OUT : CAM_DIR_NONE) |
                       (nsegs ? CAM_DATA_SG_PADDR : 0);
    ccb->csio.data_ptr = nsegs ? (uint8_t *)t->segs : NULL;
    ccb->csio.sglist_cnt = (uint16_t)nsegs;
    ccb->csio.dxfer_len = (uint32_t)len;
    ccb->csio.sense_len = SSD_FULL_SIZE;
    ccb->csio.cdb_len = cdblen;
    memcpy(ccb->csio.cdb_io.cdb_bytes, cdb, cdblen);
    ccb->csio.tag_action = MSG_SIMPLE_Q_TAG;

    /*
     * Not inline_owner: ProcessParallelTask does not deliver completions
     * (no family re-entry from the submit path, as in M4); anything another
     * ccb completes here goes to the done thread call.
     */
    mps_lock(sc);
    t->state = MPSK_T_SUBMIT;
    xpt_action(ccb);
    if (t->state == MPSK_T_SYNC_DONE) {
        t->state = MPSK_T_IDLE;
        mps_kext_result(t, res);
        rc = MPSK_SUBMIT_DONE;
    } else {
        t->state = MPSK_T_POSTED;
        rc = ccb->ccb_h.xnu_held ? MPSK_SUBMIT_HELD : MPSK_SUBMIT_POSTED;
    }
    mps_unlock(sc);
    return rc;
}

void *mps_kext_next_done(struct mps_kext *k, struct mps_kext_io_result *res)
{
    IOLockLock(k->done_lock);
    struct mps_kext_task *t = k->done_head;
    if (t) {
        k->done_head = t->done_next;
        if (k->done_head == nullptr)
            k->done_tail = nullptr;
    }
    IOLockUnlock(k->done_lock);
    if (t == nullptr)
        return nullptr;
    mps_kext_result(t, res);
    t->state = MPSK_T_IDLE;
    return t->os_task;
}

int mps_kext_timeout(struct mps_kext *k, void *td, void *os_task)
{
    struct mps_softc *sc = &k->sc;
    struct mps_kext_task *t = (struct mps_kext_task *)td;
    int rc;

    if (!k->attached)
        return ENOENT;
    mps_lock(sc);
    if (t->os_task != os_task || t->state != MPSK_T_POSTED) {
        mps_unlock(sc);
        return ENOENT;                       /* already completed */
    }
    k->inline_owner = mps_os_curthread();
    rc = mps_xnu_ccb_timeout(sc, &t->ccb);
    if (rc == ENOENT && t->state == MPSK_T_POSTED) {
        /* neither held nor posted in the core: cannot happen; do not leak the task */
        mps_dprint(sc, MPS_RECOVERY | MPS_ERROR, "timed-out IO for target %u unknown to the core, "
                   "failing it\n", t->ccb.ccb_h.target_id);
        t->ccb.ccb_h.status = CAM_CMD_TIMEOUT;
        mps_kext_io_done(NULL, &t->ccb);
        rc = 0;
    }
    k->inline_owner = nullptr;
    mps_unlock(sc);
    return rc;
}

/*
 * OS task management. IOSCSIParallelFamily's *Request methods are
 * synchronous: wait for the ccb with mps_mtx dropped, pumping the reply queue
 * every 10 ms (the work loop that runs the interrupt may be gated by our
 * caller). Bounded by the core's TM timeouts (5 s abort, 30 s resets) and by
 * mps_kext_stop_waiters().
 */
int mps_kext_tm(struct mps_kext *k, uint8_t type, uint32_t tid, uint64_t lun, void *abort_td,
                void *abort_task, uint64_t abort_tag, char *why, size_t whylen)
{
    struct mps_softc *sc = &k->sc;
    struct mps_kext_tmreq *w;
    struct mps_kext_task *t = (struct mps_kext_task *)abort_td;
    int result = MPSK_TM_FAILED;

    w = (struct mps_kext_tmreq *)IOMalloc(sizeof(*w));
    if (w == nullptr) {
        snprintf(why, whylen, "no memory");
        return MPSK_TM_FAILED;
    }
    bzero(w, sizeof(*w));
    w->k = k;

    mps_lock(sc);
    if (!k->attached || k->stopping) {
        mps_unlock(sc);
        IOFree(w, sizeof(*w));
        snprintf(why, whylen, "controller stopping");
        return MPSK_TM_FAILED;
    }
    if (type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK &&
        (t == nullptr || t->os_task != abort_task || t->os_tag != abort_tag ||
         t->state != MPSK_T_POSTED)) {
        mps_unlock(sc);
        IOFree(w, sizeof(*w));
        snprintf(why, whylen, "nothing to abort (task already completed)");
        return MPSK_TM_COMPLETE;
    }
    w->path.path_id = cam_sim_path(sc->sassc->sim);
    w->path.target_id = tid;
    w->path.lun_id = lun;
    xpt_setup_ccb(&w->ccb.ccb_h, &w->path, CAM_PRIORITY_NORMAL);
    w->ccb.ccb_h.func_code = XPT_XNU_TM;
    w->ccb.ccb_h.cbfcnp = mps_kext_tm_done;
    w->ccb.ccb_h.ppriv_ptr0 = w;
    w->ccb.xtm.task_type = type;
    w->ccb.xtm.abort_ccb = (type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK) ? &t->ccb : NULL;

    k->tm_waiters++;
    xpt_action(&w->ccb);
    while (!w->done && !k->stopping) {
        (void)msleep(&k->tm_chan, &sc->mps_mtx, 0, "mpstm", 10);
        if (!w->done && !k->stopping)
            mps_intr_locked(sc);             /* the reply may wait behind a gated work loop */
    }
    if (w->done) {
        const uint32_t st = w->ccb.ccb_h.status & CAM_STATUS_MASK;
        const uint8_t rsp = w->ccb.xtm.response_code;
        if (st == CAM_REQ_CMP)
            result = MPSK_TM_COMPLETE;
        else if (st == CAM_REQ_INVALID || st == CAM_LUN_INVALID || st == CAM_FUNC_NOTAVAIL ||
                 rsp == MPI2_SCSITASKMGMT_RSP_TM_NOT_SUPPORTED || rsp == MPI2_SCSITASKMGMT_RSP_INVALID_FRAME ||
                 rsp == MPI2_SCSITASKMGMT_RSP_TM_INVALID_LUN)
            result = MPSK_TM_REJECTED;
        snprintf(why, whylen, "CAM status 0x%x, IOCStatus 0x%x, response %s, %u terminated", st,
                 w->ccb.xtm.ioc_status, mps_describe_table(mps_scsi_taskmgmt_string, rsp),
                 w->ccb.xtm.termination_count);
    } else {
        w->orphan = 1;                       /* mps_free() completes it; mps_kext_tm_done frees it */
        w = nullptr;
        snprintf(why, whylen, "controller stopping");
    }
    if (--k->tm_waiters == 0)
        wakeup(&k->tm_waiters);
    mps_unlock(sc);
    if (w)
        IOFree(w, sizeof(*w));
    return result;
}

int mps_kext_check_present(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    int present;

    if (!k->attached)
        return 0;
    mps_lock(sc);
    if ((sc->mps_flags & (MPS_FLAGS_XNU_GONE | MPS_FLAGS_XNU_SUSPENDED)) == 0)
        (void)mps_regread(sc, MPI2_DOORBELL_OFFSET);     /* all-ones: mps_xnu_gone */
    present = (sc->mps_flags & MPS_FLAGS_XNU_GONE) == 0;
    mps_unlock(sc);
    return present;
}

int mps_kext_suspend(struct mps_kext *k, uint32_t drain_ms)
{
    struct mps_softc *sc = &k->sc;
    int error;

    if (!k->attached)
        return ENXIO;
    mps_lock(sc);
    error = mps_xnu_suspend(sc, drain_ms);
    mps_unlock(sc);
    return error;
}

int mps_kext_resume(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    int error;

    if (!k->attached)
        return ENXIO;
    mps_lock(sc);
    error = mps_xnu_resume(sc);
    mps_unlock(sc);
    return error;
}

int mps_kext_target_info(struct mps_kext *k, uint32_t tid, struct mps_kext_target_info *info)
{
    struct mps_softc *sc = &k->sc;
    int found;

    bzero(info, sizeof(*info));
    if (!k->attached)
        return 0;
    mps_lock(sc);
    found = mps_xnu_target_info(sc, tid, &info->sas_address, &info->device_info, &info->link_rate);
    mps_unlock(sc);
    return found;
}

void mps_kext_controller_info(struct mps_kext *k, struct mps_kext_controller_info *info)
{
    struct mps_softc *sc = &k->sc;

    bzero(info, sizeof(*info));
    if (!k->attached)
        return;
    mps_lock(sc);
    strlcpy(info->fw_version, sc->fw_version, sizeof(info->fw_version));
    strlcpy(info->msg_version, sc->msg_version, sizeof(info->msg_version));
    if (sc->facts != NULL) {
        info->product_id = le16toh(sc->facts->ProductID);
        info->num_ports = sc->facts->NumberOfPorts;
    }
    if (sc->sassc != NULL) {
        for (u_int i = 0; i < sc->sassc->maxtargets; i++)
            if (sc->sassc->targets[i].handle != 0 && sc->sassc->targets[i].linkrate > info->max_link_rate)
                info->max_link_rate = (uint8_t)sc->sassc->targets[i].linkrate;
    }
    mps_unlock(sc);
}

int mps_kext_target_present(struct mps_kext *k, uint32_t tid)
{
    int present;
    if (!k->attached)
        return 0;
    mps_lock(&k->sc);
    present = mps_xnu_target_present(&k->sc, tid, NULL);
    mps_unlock(&k->sc);
    return present;
}

void mps_kext_snapshot(struct mps_kext *k, struct mps_kext_snapshot *s)
{
    struct mps_softc *sc = &k->sc;
    struct mps_command *cm;
    int n = 0;

    bzero(s, sizeof(*s));
    if (k->detached) {                       /* mps_free ran: nothing else touches it */
        s->st = sc->xnu.stats;
        s->gone = 1;
        return;
    }
    if (!k->attached)
        return;
    mps_lock(sc);
    mps_xnu_stats_update(sc);
    s->st = sc->xnu.stats;
    if (sc->sassc != NULL && sc->sassc->sim != NULL)
        s->held = (uint32_t)cam_xnu_held_count(sc->sassc->sim);
    TAILQ_FOREACH(cm, &sc->high_priority_req_list, cm_link)
        n++;
    s->tms_busy = sc->num_prireqs > n ? (uint32_t)(sc->num_prireqs - n) : 0;
    s->gone = (sc->mps_flags & MPS_FLAGS_XNU_GONE) != 0;
    s->resetting = (sc->mps_flags & MPS_FLAGS_DIAGRESET) != 0 || sc->xnu.reinit_pending;
    mps_unlock(sc);
}

void mps_kext_reset_stats(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    if (!k->attached)
        return;
    mps_lock(sc);
    bzero(&sc->xnu.stats, sizeof(sc->xnu.stats));
    sc->chain_alloc_fail = 0;
    sc->chain_free_lowwater = sc->chain_free;
    if (sc->sassc != NULL && sc->sassc->sim != NULL)
        sc->sassc->sim->held_total = 0;
    mps_unlock(sc);
}

void mps_kext_set_debug(struct mps_kext *k, uint32_t mask)
{
    k->sc.mps_debug = mask & MPS_DEBUG_ALL;
}

uint32_t mps_kext_level_mask(int level)
{
    return mps_xnu_level_mask(level);
}

const char *mps_kext_level_name(int level)
{
    return mps_xnu_level_name(level);
}

int mps_kext_parse_level(const char *s)
{
    return mps_xnu_parse_level(s);
}

/* FreeBSD's parser works on a softc: run it on a scratch one. */
void mps_kext_parse_debug(const char *s, uint32_t *mask)
{
    char buf[128];
    struct mps_softc *tmp = (struct mps_softc *)IOMalloc(sizeof(*tmp));
    if (tmp == nullptr)
        return;
    bzero(tmp, sizeof(*tmp));
    tmp->mps_debug = *mask;
    strlcpy(buf, s, sizeof(buf));
    mps_parse_debug(tmp, buf);
    *mask = (uint32_t)tmp->mps_debug & MPS_DEBUG_ALL;
    IOFree(tmp, sizeof(*tmp));
}

void mps_kext_describe_debug(uint32_t mask, char *buf, size_t len)
{
    mps_xnu_describe_debug(mask, buf, len);
}

const char *mps_kext_tm_type_str(uint8_t type)
{
    return mps_xnu_tm_type_str(type);
}

} /* extern "C" */
