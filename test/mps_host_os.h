/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_host_os.h - the mps_os_* hooks on the host, for mps_port_test.cpp.
 *
 * Single-threaded model of the kernel: a virtual clock (the mock IOC's
 * nowUs), registers and DMA forwarded to a MockIocModel, and "other
 * threads" (interrupts, callouts, taskqueues) run by HostRunPending()
 * whenever the core waits with no lock held (msleep, DELAY >= 1 ms) or the
 * test advances time.  Include after mock_ioc.h and the core headers.
 */
#ifndef MPS_HOST_OS_H
#define MPS_HOST_OS_H

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct MockIocModel;
struct mps_softc;
struct mtx;
struct taskqueue;

struct HostLogRec {
    uint32_t cls;
    std::string line;
};

/*
 * A thread_call with xnu semantics (TF1): enter returns true if the call
 * was already queued (no new invocation); an enter while it runs queues a
 * second invocation, which HostRunPending starts at once, nested ("another
 * thread", concurrently); cancel dequeues a queued invocation only.
 */
struct HostThreadCall {
    std::function<void()> fn;
    bool queued = false;
    int running = 0;
    int invocations = 0, concurrent = 0;
    bool Enter() { if (queued) return true; queued = true; return false; }
    bool Cancel() { if (!queued) return false; queued = false; return true; }
};

/* kext/Info.plist's personality keys that are core tunables (mps.max_io_pages, D32) */
static const int kMaxIoPages = 256;
static const std::map<std::string, std::string> kInfoPlistTunables = {
    {"mps.max_io_pages", "256"},
};

struct MpsHostOs {
    std::vector<HostThreadCall *> threadCalls;  /* run by HostRunPending */
    MockIocModel *ioc = nullptr;
    struct mps_softc *sc = nullptr;         /* interrupts go to mps_intr_msi(sc) */
    bool verbose = false;

    /* locks / sleep */
    int locksHeld = 0;
    std::set<void *> woken;
    int sleeps = 0, delays = 0;
    std::function<void()> onOtherThreads;   /* test hook, runs inside HostRunPending */
    std::function<void(uint64_t)> onDelay;  /* test hook, runs at the start of every DELAY/pause */
    /* mps_os_getenv: boot-args / personality; the defaults are kext/Info.plist's */
    std::map<std::string, std::string> tunables = kInfoPlistTunables;

    /* callouts, taskqueues */
    std::set<struct mtx *> calloutMtx;      /* mutexes with an armed deadline */
    std::vector<std::pair<struct mtx *, uint64_t>> armed;
    std::set<struct taskqueue *> queues, kicked, running;
    /*
     * xnu thread_call semantics: a queue kicked while it runs starts another
     * invocation ("another thread") at once; compat must serialize the tasks.
     */
    bool threadCallQueues = true;
    int queueReentries = 0;                 /* invocations started while one ran */

    /* DMA */
    int dmaAllocs = 0, dmaMultiSeg = 0, maxSegs = 0;

    /* allocation failure injection (TF6): malloc + DMA allocations counted together */
    long allocs = 0;
    long failAllocAt = 0;                   /* fail allocation number failAllocAt (0: never) */
    int allocFailures = 0;
    bool AllocFails() {
        allocs++;
        if (failAllocAt != 0 && allocs == failAllocAt) { allocFailures++; return true; }
        return false;
    }

    /* platform events */
    int interruptsSetUp = 0, interruptsFreed = 0, pciRestores = 0, dead = 0;
    int panicsAllowed = 0, panics = 0;      /* a test that provokes panic() (D18): it returns, as on xnu */
    int interruptsDelivered = 0;

    /* log */
    std::vector<HostLogRec> log;
    size_t Mark() const { return log.size(); }
    int Count(size_t mark, const char *sub) const;
    bool Has(size_t mark, const char *sub) const { return Count(mark, sub) > 0; }
    void DumpTail(size_t n = 40) const;
};

extern MpsHostOs gHost;

/* Run interrupts, due callouts and kicked taskqueues until nothing is left. */
void HostRunPending();
/* Advance the virtual clock by `ms' in 1 ms steps, running what becomes due. */
void HostAdvance(uint64_t ms);

#endif /* MPS_HOST_OS_H */
