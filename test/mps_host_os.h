/*
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

struct MpsHostOs {
    MockIocModel *ioc = nullptr;
    struct mps_softc *sc = nullptr;         /* interrupts go to mps_intr_msi(sc) */
    bool verbose = false;

    /* locks / sleep */
    int locksHeld = 0;
    std::set<void *> woken;
    int sleeps = 0, delays = 0;
    std::function<void()> onOtherThreads;   /* test hook, runs inside HostRunPending */

    /* callouts, taskqueues */
    std::set<struct mtx *> calloutMtx;      /* mutexes with an armed deadline */
    std::vector<std::pair<struct mtx *, uint64_t>> armed;
    std::set<struct taskqueue *> queues, kicked, running;

    /* DMA */
    int dmaAllocs = 0, dmaMultiSeg = 0, maxSegs = 0;

    /* platform events */
    int interruptsSetUp = 0, interruptsFreed = 0, pciRestores = 0, dead = 0;
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
