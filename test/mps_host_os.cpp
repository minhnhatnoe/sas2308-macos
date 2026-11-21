/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_host_os.cpp - mps_os_* on the host (see mps_host_os.h).
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void HostFail(const char *what);
#define CHECK(c) do { if (!(c)) HostFail(#c); } while (0)
#include "mock_ioc.h"

#define MPS_COMPAT_NO_LIBC_REMAP
extern "C" {
#include "../core/compat/mps_compat.h"
#include "../core/compat/cam_compat.h"
#include "../core/mps_mpi.h"
#include "../core/mpsvar.h"
#include "../core/mps_xnu.h"
}
#include "mps_host_os.h"

MpsHostOs gHost;

static void HostFail(const char *what)
{
    printf("FAIL (host os): %s\n", what);
    gHost.DumpTail();
    exit(1);
}

int MpsHostOs::Count(size_t mark, const char *sub) const
{
    int n = 0;
    for (size_t i = mark; i < log.size(); i++)
        if (log[i].line.find(sub) != std::string::npos) n++;
    return n;
}

void MpsHostOs::DumpTail(size_t n) const
{
    size_t from = log.size() > n ? log.size() - n : 0;
    printf("  last %zu log lines:\n", log.size() - from);
    for (size_t i = from; i < log.size(); i++)
        printf("    | [%s] %s\n", mps_xnu_category(log[i].cls), log[i].line.c_str());
}

/* ---- "other threads" ---------------------------------------------------------- */

/*
 * May nest: a task that sleeps (the reinit) lets the other "threads" run;
 * a queue kicked while it runs gets a second invocation (xnu thread_call,
 * threadCallQueues) and nothing runs while a lock is held.
 */
void HostRunPending()
{
    if (gHost.locksHeld != 0)
        return;
    for (int round = 0; round < 10000; round++) {
        bool progress = false;
        if (gHost.sc && gHost.ioc && gHost.ioc->InterruptPending()) {
            uint32_t before = gHost.ioc->hostPost;
            gHost.interruptsDelivered++;
            mps_intr_msi(gHost.sc);
            /* a reset in progress leaves the queue alone: not progress */
            progress |= gHost.ioc->hostPost != before;
        }
        for (size_t i = 0; i < gHost.armed.size(); i++) {
            if (gHost.armed[i].second <= gHost.ioc->nowUs) {
                struct mtx *m = gHost.armed[i].first;
                gHost.armed.erase(gHost.armed.begin() + i);
                mps_compat_callout_run(m);
                progress = true;
                break;
            }
        }
        for (struct taskqueue *tq : std::vector<struct taskqueue *>(gHost.kicked.begin(), gHost.kicked.end())) {
            if (!gHost.queues.count(tq))
                continue;
            const bool nested = gHost.running.count(tq) != 0;
            if (nested && !gHost.threadCallQueues)
                continue;
            gHost.kicked.erase(tq);
            if (nested)
                gHost.queueReentries++;
            else
                gHost.running.insert(tq);
            mps_compat_taskqueue_run(tq);
            if (!nested)
                gHost.running.erase(tq);
            progress = true;
        }
        for (HostThreadCall *tc : std::vector<HostThreadCall *>(gHost.threadCalls)) {
            if (!tc->queued)
                continue;
            tc->queued = false;
            if (tc->running)
                tc->concurrent++;
            tc->running++;
            tc->invocations++;
            tc->fn();
            tc->running--;
            progress = true;
        }
        if (gHost.onOtherThreads)
            gHost.onOtherThreads();
        if (!progress)
            break;
    }
}

void HostAdvance(uint64_t ms)
{
    for (uint64_t i = 0; i < ms; i++) {
        gHost.ioc->nowUs += 1000;
        HostRunPending();
    }
}

/* ---- hooks ------------------------------------------------------------------------ */

struct HostLock {
    const char *name;
    bool held;
};

extern "C" {

void *mps_os_malloc(size_t size, int canwait)
{
    (void)canwait;
    if (gHost.AllocFails())
        return nullptr;
    return malloc(size);
}

void mps_os_free(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}

void *mps_os_lock_alloc(const char *name)
{
    return new HostLock{name, false};
}

void mps_os_lock_free(void *lock)
{
    CHECK(!((HostLock *)lock)->held);
    delete (HostLock *)lock;
}

void mps_os_lock(void *lock)
{
    HostLock *l = (HostLock *)lock;
    if (l->held) {
        printf("FAIL: lock %s taken twice (deadlock on a real kernel)\n", l->name);
        gHost.DumpTail();
        exit(1);
    }
    l->held = true;
    gHost.locksHeld++;
}

void mps_os_unlock(void *lock)
{
    HostLock *l = (HostLock *)lock;
    CHECK(l->held);
    l->held = false;
    gHost.locksHeld--;
}

void *mps_os_curthread(void)
{
    return (void *)&gHost;
}

int mps_os_sleep(void *lock, void *chan, uint64_t timo_us)
{
    gHost.sleeps++;
    /*
     * msleep drops the lock and sleeps atomically: a wakeup issued before
     * (with the lock held, or earlier) is lost on a real kernel.
     */
    gHost.woken.erase(chan);
    mps_os_unlock(lock);
    gHost.ioc->nowUs += 100;
    HostRunPending();
    int rc;
    if (gHost.woken.erase(chan)) {
        rc = 0;
    } else {
        CHECK(timo_us != 0);            /* nobody will wake us: a hang */
        /*
         * Time passes in 1 ms steps, so callouts and other threads due
         * meanwhile run (and may wake us) before the timeout, as on a
         * real kernel.
         */
        for (uint64_t left = timo_us; left > 0 && !gHost.woken.count(chan);) {
            const uint64_t step = left < 1000 ? left : 1000;
            gHost.ioc->nowUs += step;
            left -= step;
            HostRunPending();
        }
        rc = gHost.woken.erase(chan) ? 0 : EWOULDBLOCK;
    }
    mps_os_lock(lock);
    return rc;
}

void mps_os_wakeup(void *chan)
{
    gHost.woken.insert(chan);
}

void mps_os_delay(uint64_t us)
{
    gHost.delays++;
    if (gHost.onDelay)
        gHost.onDelay(us);
    gHost.ioc->nowUs += us;
    HostRunPending();                   /* no-op while a lock is held */
}

uint64_t mps_os_uptime_us(void)
{
    return gHost.ioc->nowUs;
}

void mps_os_walltime(uint64_t *sec, uint32_t *usec)
{
    *sec = 1700000000ull + gHost.ioc->nowUs / 1000000;
    *usec = (uint32_t)(gHost.ioc->nowUs % 1000000);
}

uint32_t mps_os_bus_read_4(bus_space_tag_t, bus_space_handle_t, bus_size_t off)
{
    return gHost.ioc->RegRead((uint32_t)off);
}

void mps_os_bus_write_4(bus_space_tag_t, bus_space_handle_t, bus_size_t off, uint32_t val)
{
    gHost.ioc->RegWrite((uint32_t)off, val);
}

struct HostDma {
    void *cpu;
    size_t size;
    std::vector<uint64_t> pieces;       /* IOVA of each piece */
    size_t piece;
};

int mps_os_dmamem_alloc(bus_dma_tag_t tag, void **vaddr, void **cookie)
{
    if (gHost.AllocFails())
        return ENOMEM;
    HostDma *d = new HostDma;
    d->size = (size_t)tag->dt_maxsize;
    CHECK(d->size > 0 && d->size < (64u << 20));
    /* several segments allowed: map it in 4 KiB pieces (like a DART would) */
    d->piece = (tag->dt_nsegments > 1) ? 4096 : 0;
    uint64_t iova;
    d->cpu = gHost.ioc->DmaAlloc(d->size, (size_t)std::max<bus_size_t>(tag->dt_alignment, 16),
                                 &iova, d->piece, &d->pieces);
    if (d->piece == 0)
        d->pieces.push_back(iova);
    for (uint64_t a : d->pieces) {
        CHECK(a % tag->dt_alignment == 0);
        CHECK(a + (d->piece ? d->piece : d->size) - 1 <= tag->dt_lowaddr);
    }
    gHost.dmaAllocs++;
    if (d->pieces.size() > 1)
        gHost.dmaMultiSeg++;
    *vaddr = d->cpu;
    *cookie = d;
    return 0;
}

void mps_os_dmamem_free(bus_dma_tag_t, void *vaddr, void *cookie)
{
    HostDma *d = (HostDma *)cookie;
    CHECK(d->cpu == vaddr);
    gHost.ioc->DmaFree(d->cpu);
    delete d;
}

int mps_os_dmamem_load(bus_dma_tag_t tag, void *cookie, bus_dma_segment_t *segs, int maxsegs, int *nsegs)
{
    HostDma *d = (HostDma *)cookie;
    int n = 0;
    for (size_t i = 0; i < d->pieces.size(); i++) {
        if (n >= maxsegs)
            return EFBIG;
        size_t len = d->piece ? std::min(d->piece, d->size - i * d->piece) : d->size;
        CHECK(len <= tag->dt_maxsegsz);
        segs[n].ds_addr = d->pieces[i];
        segs[n].ds_len = len;
        n++;
    }
    *nsegs = n;
    gHost.maxSegs = std::max(gHost.maxSegs, n);
    return 0;
}

void mps_os_dmamem_unload(bus_dma_tag_t, void *)
{
}

void mps_os_callout_arm(struct mtx *m, uint64_t deadline_us)
{
    for (size_t i = 0; i < gHost.armed.size(); i++)
        if (gHost.armed[i].first == m) { gHost.armed.erase(gHost.armed.begin() + i); break; }
    if (deadline_us != 0)
        gHost.armed.push_back({m, deadline_us});
}

int mps_os_taskqueue_start(struct taskqueue *tq)
{
    gHost.queues.insert(tq);
    return 0;
}

void mps_os_taskqueue_kick(struct taskqueue *tq)
{
    gHost.kicked.insert(tq);
}

void mps_os_taskqueue_wait(struct taskqueue *tq)
{
    CHECK(!gHost.running.count(tq));    /* draining from inside the queue */
    gHost.kicked.erase(tq);
    gHost.running.insert(tq);
    mps_compat_taskqueue_run(tq);
    gHost.running.erase(tq);
}

void mps_os_taskqueue_stop(struct taskqueue *tq)
{
    CHECK(!gHost.running.count(tq));
    gHost.queues.erase(tq);
    gHost.kicked.erase(tq);
}

void mps_os_log(device_t, uint32_t cls, const char *line)
{
    CHECK(line && strlen(line) < 320 && !strchr(line, '\n'));
    gHost.log.push_back({cls, line});
    if (gHost.verbose)
        printf("    | [%s] %s\n", mps_xnu_category(cls), line);
}

void mps_os_assert_fail(const char *msg)
{
    if (gHost.panicsAllowed > 0) {
        gHost.panicsAllowed--;
        gHost.panics++;
        return;
    }
    printf("FAIL: KASSERT/panic: %s\n", msg);
    gHost.DumpTail();
    exit(1);
}

int mps_os_setup_interrupts(device_t)
{
    gHost.interruptsSetUp++;
    return 0;
}

void mps_os_free_interrupts(device_t)
{
    gHost.interruptsFreed++;
}

int mps_os_pci_restore(device_t)
{
    gHost.pciRestores++;
    return 0;
}

void mps_os_controller_dead(device_t)
{
    gHost.dead++;
}

int mps_os_pci_present(device_t)
{
    return gHost.ioc == nullptr || !gHost.ioc->dead;    /* config space: only an unplug */
}

} /* extern "C" */
