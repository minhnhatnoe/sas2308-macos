/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mock_ioc_test.cpp - the scenario groups M1-M4 (named after the development
 * milestones, docs/design.md) against the FreeBSD-shaped core
 * (core/mps*.c behind core/compat), driven the way the kext drives it: SCSI
 * IO and task management as ccbs through xpt_action(), OS timeouts through
 * mps_xnu_ccb_timeout(), interrupts / callouts / taskqueues as "other
 * threads" of the host model (mps_host_os.cpp), the IOC as the software
 * model in mock_ioc.h.  Every ccb must complete exactly once (checked when
 * each rig detaches).  Built with ASan/UBSan/LSan.
 *
 *   make -C test        MOCK_VERBOSE=1 prints every log line
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "mps_host_os.h"

static void Fail(const char *file, int line, const char *expr);
#define CHECK(c) do { if (!(c)) Fail(__FILE__, __LINE__, #c); } while (0)
#include "mock_ioc.h"

#define MPS_COMPAT_NO_LIBC_REMAP
extern "C" {
#include "../core/compat/mps_compat.h"
#include "../core/compat/cam_compat.h"
#include "../core/mps_mpi.h"
#include "../core/mpsvar.h"
#include "../core/mps_sas.h"
#include "../core/mps_table.h"
#include "../core/mps_xnu.h"
}

static void Fail(const char *file, int line, const char *expr)
{
    printf("FAIL %s:%d: %s\n", file, line, expr);
    gHost.DumpTail();
    exit(1);
}

#ifdef REVIEW_BEFORE                            /* the review tests against the code before the fixes */
#define XSUSPEND(sc, ms) mps_xnu_suspend(sc, ms)
#else
#define XSUSPEND(sc, ms) mps_xnu_suspend(sc, ms, 0)
#endif

static size_t LogMark() { return gHost.Mark(); }
static int CountLog(size_t mark, const char *sub) { return gHost.Count(mark, sub); }
static bool HasLog(size_t mark, const char *sub) { return gHost.Has(mark, sub); }
static uint32_t LogClass(size_t mark, const char *sub)
{
    for (size_t i = mark; i < gHost.log.size(); i++)
        if (gHost.log[i].line.find(sub) != std::string::npos) return gHost.log[i].cls;
    return 0;
}

static const uint8_t kAbortT = MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK;
static const uint8_t kLunT = MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET;
static const uint8_t kTgtT = MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET;
static const uint32_t kMaxSegBytes = 0xFFFFFF;  /* buffer_dmat maxsegsz (24 bit) */

/* xorshift32: fixed-seed fuzz, same sequence on every platform. */
static uint32_t Rand(uint32_t &x)
{
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

/* Fake DMA memory from the model (the OS's IODMACommand segments). */
struct Buf { uint8_t *cpu = nullptr; uint64_t iova = 0; size_t size = 0; };
static Buf AllocBuf(MockIocModel &m, size_t size)
{
    Buf b;
    b.cpu = (uint8_t *)m.DmaAlloc(size, 4096, &b.iova);
    b.size = size;
    return b;
}
static void FreeBuf(MockIocModel &m, Buf &b) { m.DmaFree(b.cpu); b = Buf(); }

/*
 * Segments of the given lengths in `buf' with random gaps, handed over in a
 * shuffled order (the SGL order must be the list order, not address order).
 */
static std::vector<MockSeg> Scatter(const Buf &buf, const std::vector<uint32_t> &lens, uint32_t &rng)
{
    std::vector<MockSeg> segs;
    uint64_t off = 0;
    for (uint32_t len : lens) {
        off += Rand(rng) % 64;
        CHECK(off + len <= buf.size);
        segs.push_back(MockSeg{buf.iova + off, len});
        off += len;
    }
    for (size_t i = segs.size(); i > 1; i--) {
        size_t j = Rand(rng) % i;
        MockSeg t = segs[i - 1]; segs[i - 1] = segs[j]; segs[j] = t;
    }
    return segs;
}

static uint8_t *CpuOf(const Buf &buf, const MockSeg &seg)
{
    CHECK(seg.iova >= buf.iova && seg.iova + seg.len <= buf.iova + buf.size);
    return buf.cpu + (seg.iova - buf.iova);
}

/*
 * Chain frames FreeBSD's mps_push_sge needs for n segments (frame = bytes):
 * the request frame's 32-byte SGL takes 1 SGE (+ chain), each chain frame
 * frame/12 - 1 (+ chain).
 */
static uint32_t ChainsFor(uint32_t n, uint32_t frame = 128)
{
    const uint32_t per = frame / 12 - 1;
    return n <= 1 ? 0 : (n - 1 + per - 1) / per;
}

/* ---- the OS side: one SCSI IO / task management request = one ccb ---------- */

struct Io {
    union ccb *ccb = nullptr;
    std::vector<bus_dma_segment_t> sg;
    int done = 0;
    uint32_t status = 0, fullStatus = 0;    /* CAM status at completion */
    uint8_t scsiStatus = 0;
    uint32_t resid = 0, senseLen = 0;
    uint8_t sense[32]{};
    uint16_t smid = 0;                      /* SMID it was posted with (0: never) */
    uint64_t startUs = 0, doneUs = 0;
    int deadAtDone = -1;                    /* gHost.dead (bus mastering off) when it completed */
};

static uint64_t gNowUs();

static void IoDone(struct cam_periph *, union ccb *ccb)
{
    Io *io = (Io *)ccb->ccb_h.ppriv_ptr0;
    CHECK(io->done == 0);                   /* every ccb completes exactly once */
    io->done++;
    io->deadAtDone = gHost.dead;
    io->fullStatus = ccb->ccb_h.status;
    io->status = ccb->ccb_h.status & CAM_STATUS_MASK;
    io->doneUs = gNowUs();
    if (ccb->ccb_h.func_code != XPT_SCSI_IO)
        return;
    io->scsiStatus = ccb->csio.scsi_status;
    io->resid = ccb->csio.resid;
    if (ccb->ccb_h.status & CAM_AUTOSNS_VALID) {
        io->senseLen = ccb->csio.sense_len - ccb->csio.sense_resid;
        memcpy(io->sense, &ccb->csio.sense_data, io->senseLen < sizeof(io->sense) ? io->senseLen : sizeof(io->sense));
    }
}

enum St { kPosted, kHeld, kDone };

struct AsyncRec { uint32_t code; target_id_t tid; int fc; };

/* A controller: model + softc, attached; nested rigs park the outer one. */
struct Rig {
    MockIocModel ioc;
    struct device dev{};
    struct mps_softc *sc = nullptr;
    int attachRc = -1;
    std::vector<Io *> ios;
    std::vector<AsyncRec> async;
    /* parked state of the enclosing rig */
    MockIocModel *prevIoc;
    struct mps_softc *prevSc;
    std::vector<std::pair<struct mtx *, uint64_t>> prevArmed;
    long mallocBase;
    static int units;
    static long failAllocN;                 /* TF6: fail the Nth allocation of mps_attach */

    explicit Rig(std::vector<uint16_t> devs = {0x0009, 0x000A},
                 std::function<void(Rig &)> pre = nullptr, bool start = true) {
        prevIoc = gHost.ioc; prevSc = gHost.sc; prevArmed.swap(gHost.armed);
        mallocBase = mps_compat_malloc_count();
        ioc.devices = devs;
        ioc.maxTargets = 32;
        ioc.strictEventMasks = false;       /* mpssas registers IR events too */
        ioc.tmSmidLo = 1;                   /* high-priority SMIDs 1..num_prireqs */
        ioc.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
        if (prevIoc) ioc.nowUs = prevIoc->nowUs;
        gHost.ioc = &ioc;
        sc = (struct mps_softc *)calloc(1, sizeof(*sc));
        gHost.sc = sc;
        mps_compat_device_init(&dev, "mps", units++, sc, &ioc, 0x0087);
        sc->mps_dev = &dev;
        mps_get_tunables(sc);
        sc->mps_debug = MPS_DEBUG_ALL;      /* every line formatted (ASan/UBSan check them) */
        if (pre) pre(*this);
        CHECK(bus_dma_tag_create(bus_get_dma_tag(&dev), 1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
                                 NULL, NULL, BUS_SPACE_MAXSIZE_32BIT, BUS_SPACE_UNRESTRICTED,
                                 BUS_SPACE_MAXSIZE_32BIT, 0, NULL, NULL, &sc->mps_parent_dmat) == 0);
        if (failAllocN) gHost.failAllocAt = gHost.allocs + failAllocN;
        attachRc = mps_attach(sc);
        gHost.failAllocAt = 0;
        if (attachRc != 0) return;
        struct cam_path *path;
        CHECK(xpt_create_path(&path, NULL, cam_sim_path(sc->sassc->sim), CAM_TARGET_WILDCARD,
                              CAM_LUN_WILDCARD) == CAM_REQ_CMP);
        CHECK(xpt_register_async(AC_XNU_SCAN | AC_BUS_RESET | AC_SENT_BDR | AC_FOUND_DEVICE |
                                 AC_LOST_DEVICE, AsyncCb, this, path) == CAM_REQ_CMP);
        xpt_free_path(path);
        if (start) Start();
    }
    ~Rig() {
        Detach();
        for (Io *io : ios) delete io;
    }
    void Start() {                          /* config_intrhook: mps_startup, port enable */
        mps_compat_intrhook_run(sc);
        HostRunPending();
    }
    static void AsyncCb(void *arg, uint32_t code, struct cam_path *path, void *a) {
        AsyncRec r{code, path ? path->target_id : CAM_TARGET_WILDCARD, -1};
        if (code == AC_XNU_SCAN) r.fc = ((union ccb *)a)->ccb_h.func_code;
        ((Rig *)arg)->async.push_back(r);
    }
    int Scans(target_id_t tid) const {
        int n = 0;
        for (const AsyncRec &r : async) n += r.code == AC_XNU_SCAN && r.tid == tid && r.fc == XPT_SCAN_TGT;
        return n;
    }
    int AsyncCount(uint32_t code) const {
        int n = 0;
        for (const AsyncRec &r : async) n += r.code == code;
        return n;
    }
    void Detach() {
        if (sc == nullptr) return;
        if (attachRc == 0) CHECK(mps_free(sc) == 0);   /* a failed mps_attach freed itself */
        CHECK(bus_dma_tag_destroy(sc->mps_parent_dmat) == 0);
        for (Io *io : ios) {
            CHECK(io->done == 1);           /* every ccb completed exactly once */
            xpt_free_path(io->ccb->ccb_h.path);
            xpt_free_ccb(io->ccb);
            io->ccb = nullptr;              /* the record stays for the checks */
        }
        free(sc);
        sc = nullptr;
        if (rbuf.cpu) FreeBuf(ioc, rbuf);
        CHECK(ioc.liveAllocs == 0);         /* every DMA allocation returned */
        CHECK(mps_compat_malloc_count() == mallocBase);
        if (prevIoc && prevIoc->nowUs < ioc.nowUs) prevIoc->nowUs = ioc.nowUs;
        gHost.ioc = prevIoc; gHost.sc = prevSc; gHost.armed.swap(prevArmed);
        gHost.onOtherThreads = nullptr;
    }

    /* ---- queries ---- */
    struct mpssas_target *Targ(uint16_t handle) {
        for (u_int i = 0; i < sc->sassc->maxtargets; i++)
            if (sc->sassc->targets[i].handle == handle) return &sc->sassc->targets[i];
        return nullptr;
    }
    target_id_t Tid(uint16_t handle) {
        struct mpssas_target *t = Targ(handle);
        CHECK(t != nullptr);
        return t->tid;
    }
    bool Present(u_int tid, uint16_t handle) {
        uint16_t h = 0;
        return mps_xnu_target_present(sc, tid, &h) && h == handle;
    }
    int TmsBusy() {
        int n = 0;
        struct mps_command *cm;
        TAILQ_FOREACH(cm, &sc->high_priority_req_list, cm_link) n++;
        return sc->num_prireqs - n;
    }
    int HeldCount() { return cam_xnu_held_count(sc->sassc->sim); }
    bool Recovering(uint16_t handle) {
        struct mpssas_target *t = Targ(handle);
        return t && (t->tm != nullptr || !TAILQ_EMPTY(&t->timedout_commands));
    }
    int Outstanding(uint16_t handle, int lun = -1) {
        struct mpssas_target *t = Targ(handle);
        int n = 0;
        struct mps_command *cm;
        TAILQ_FOREACH(cm, &t->commands, cm_link) n += lun < 0 || cm->cm_lun == (u_int)lun;
        return n;
    }
    uint16_t SmidOf(Io *io) {
        struct mps_command *cm = (struct mps_command *)io->ccb->ccb_h.spriv_ptr1;
        return (cm && cm->cm_ccb == io->ccb && cm->cm_state == MPS_CM_STATE_INQUEUE) ?
            cm->cm_desc.Default.SMID : 0;
    }
    bool IoOutstanding(Io *io) { return SmidOf(io) != 0; }
    bool Gone() { return sc->mps_flags & MPS_FLAGS_XNU_GONE; }
    bool InReset() { return sc->mps_flags & MPS_FLAGS_DIAGRESET; }
    bool ReinitQueued() { return sc->xnu.reinit_pending; }
    bool Faulted() {
        return (ioc.state & MPI2_IOC_STATE_MASK) == MPI2_IOC_STATE_FAULT && sc->xnu.reinit_pending;
    }
    uint32_t MaxSegs() { return (uint32_t)sc->buffer_dmat->dt_nsegments; }
    int ChainsFree() { return sc->chain_free; }
    int ChainsTotal() { return sc->num_chains; }
    const struct mps_xnu_stats &Stats() {
        mps_lock(sc);
        mps_xnu_stats_update(sc);
        mps_unlock(sc);
        return sc->xnu.stats;
    }

    /* ---- the OS submitting work ---- */
    Io *NewIo(target_id_t tid, uint64_t lun, uint32_t dir, const uint8_t *cdb, uint8_t cdbLen,
              const std::vector<MockSeg> &segs, uint32_t timeoutMs = 0) {
        Io *io = new Io;
        ios.push_back(io);
        union ccb *ccb = xpt_alloc_ccb();
        io->ccb = ccb;
        CHECK(xpt_create_path(&ccb->ccb_h.path, NULL, cam_sim_path(sc->sassc->sim), tid, lun) == CAM_REQ_CMP);
        xpt_setup_ccb(&ccb->ccb_h, ccb->ccb_h.path, CAM_PRIORITY_NORMAL);
        ccb->ccb_h.func_code = XPT_SCSI_IO;
        ccb->ccb_h.cbfcnp = IoDone;
        ccb->ccb_h.ppriv_ptr0 = io;
        ccb->ccb_h.timeout = timeoutMs;
        uint64_t total = 0;
        for (const MockSeg &s : segs) {
            io->sg.push_back(bus_dma_segment_t{s.iova, s.len});
            total += s.len;
        }
        ccb->ccb_h.flags = dir | (segs.empty() ? 0 : CAM_DATA_SG_PADDR);
        ccb->csio.data_ptr = segs.empty() ? nullptr : (uint8_t *)io->sg.data();
        ccb->csio.sglist_cnt = (uint16_t)segs.size();
        ccb->csio.dxfer_len = (uint32_t)total;
        ccb->csio.sense_len = SSD_FULL_SIZE;
        ccb->csio.cdb_len = cdbLen;
        memcpy(ccb->csio.cdb_io.cdb_bytes, cdb, cdbLen);
        ccb->csio.tag_action = MSG_SIMPLE_Q_TAG;
        return io;
    }
    St Go(Io *io) {
        mps_lock(sc);
        io->startUs = ioc.nowUs;
        xpt_action(io->ccb);
        io->smid = SmidOf(io);
        mps_unlock(sc);
        return io->done ? kDone : io->ccb->ccb_h.xnu_held ? kHeld : kPosted;
    }
    /* A READ(10) of 4 KiB from `buf' (slot = seed % 16), as the milestone-M4 rig did. */
    Buf rbuf;
    Io *Read(uint16_t handle, uint64_t lun = 0, uint8_t seed = 7) {
        uint8_t cdb[10] = {0x28, 0, 0, 0, 0, seed, 0, 0, 8, 0};
        if (rbuf.cpu == nullptr) rbuf = AllocBuf(ioc, 16 * 4096);
        return NewIo(Tid(handle), lun, CAM_DIR_IN, cdb, 10,
                     {MockSeg{rbuf.iova + (uint64_t)(seed % 16) * 4096, 4096}});
    }
    /* the OS's target or LUN reset (D10): XPT_RESET_DEV, as CAM sends it */
    Io *ResetDev(target_id_t tid) {
        Io *io = new Io;
        ios.push_back(io);
        union ccb *ccb = xpt_alloc_ccb();
        io->ccb = ccb;
        CHECK(xpt_create_path(&ccb->ccb_h.path, NULL, cam_sim_path(sc->sassc->sim), tid, 0) == CAM_REQ_CMP);
        xpt_setup_ccb(&ccb->ccb_h, ccb->ccb_h.path, CAM_PRIORITY_NORMAL);
        ccb->ccb_h.func_code = XPT_RESET_DEV;
        ccb->ccb_h.cbfcnp = IoDone;
        ccb->ccb_h.ppriv_ptr0 = io;
        Go(io);
        return io;
    }
    int Timeout(Io *io) {
        mps_lock(sc);
        int rc = mps_xnu_ccb_timeout(sc, io->ccb);
        mps_unlock(sc);
        return rc;
    }

    /* ---- the "other threads" ---- */
    void Interrupt() { mps_intr_msi(sc); }      /* one MSI, even with nothing queued */
    void Pump() {                               /* interrupts only: no reinit task */
        for (int i = 0; i < 64 && ioc.InterruptPending(); i++) Interrupt();
    }
    void Callouts() { mps_compat_callout_run(&sc->mps_mtx); }   /* due callouts only */
    int RunReinit() {                           /* the reinit taskqueue (and the rest) */
        CHECK(ReinitQueued());
        HostRunPending();
        CHECK(!InReset());
        return Gone() ? ENXIO : 0;
    }
    int ForceReinit() {                         /* mps_reinit() from a thread */
        mps_lock(sc);
        int rc = (mps_reinit)(sc);
        mps_unlock(sc);
        return rc;
    }
    /* A target reset, waited for the way a family TM request does. */
    Io *ResetDevSync(target_id_t tid) {
        Io *t = ResetDev(tid);
        for (int i = 0; i < 64 && !t->done; i++) {
            Pump();
            if (!t->done) HostAdvance(100);
        }
        CHECK(t->done);
        return t;
    }
    void Escalate(Io *io) {                     /* OS timeout, recovery runs to its end */
        CHECK(Timeout(io) == 0);
        Pump();
    }
};
int Rig::units = 0;
long Rig::failAllocN = 0;

static Rig *gRig;
static uint64_t gNowUs() { return gHost.ioc ? gHost.ioc->nowUs : 0; }

/* A data IO with segments `segs' inside `buf': fill for writes, check after the post. */
static St StartData(Rig &g, uint32_t dir, const std::vector<MockSeg> &segs, const Buf &buf,
                       uint8_t seed, Io **out, uint16_t handle = 0x0009)
{
    uint8_t cdb[10] = {(uint8_t)(dir == CAM_DIR_IN ? 0x28 : 0x2A), 0, 0, 0, 0, seed, 0, 0, 0, 0};
    uint64_t total = 0, k = 0;
    for (const MockSeg &sg : segs) total += sg.len;
    for (const MockSeg &sg : segs) {
        uint8_t *d = CpuOf(buf, sg);
        for (uint32_t i = 0; i < sg.len; i++, k++) d[i] = dir == CAM_DIR_OUT ? Pat(k, seed ^ 0x5A) : 0;
    }
    Io *io = g.NewIo(g.Tid(handle), 0, dir, cdb, 10, segs);
    *out = io;
    St st = g.Go(io);
    if (st == kPosted) {
        CHECK(MockIocModel::SameSgl(g.ioc.lastSgl, segs));       /* SGL order == segs order */
        CHECK(g.ioc.lastChains == ChainsFor((uint32_t)segs.size(), g.sc->reqframesz));
        if (dir == CAM_DIR_OUT) {
            CHECK(g.ioc.lastWrite.size() == total);
            for (uint64_t i = 0; i < total; i++)
                if (g.ioc.lastWrite[i] != Pat(i, seed ^ 0x5A)) CHECK(!"write data mismatch");
        }
    }
    return st;
}

static void CheckRead(const std::vector<MockSeg> &segs, const Buf &buf, uint8_t seed)
{
    uint64_t k = 0;
    for (const MockSeg &sg : segs) {
        const uint8_t *d = CpuOf(buf, sg);
        for (uint32_t i = 0; i < sg.len; i++, k++)
            if (d[i] != Pat(k, seed)) CHECK(!"read data mismatch");
    }
}

/* One IO start-to-completion with full checks. */
static void DataIo(Rig &g, uint32_t dir, const std::vector<MockSeg> &segs, const Buf &buf, uint8_t seed)
{
    Io *io;
    CHECK(StartData(g, dir, segs, buf, seed, &io) == kPosted);
    CHECK(g.ChainsFree() == g.ChainsTotal() - (int)ChainsFor((uint32_t)segs.size(), g.sc->reqframesz));
    g.Pump();
    CHECK(io->done == 1 && io->status == CAM_REQ_CMP);
    uint64_t total = 0;
    for (const MockSeg &sg : segs) total += sg.len;
    CHECK(io->ccb->csio.dxfer_len - io->resid == total);
    CHECK(g.ChainsFree() == g.ChainsTotal());
    if (dir == CAM_DIR_IN) CheckRead(segs, buf, seed);
}

static uint64_t Fast(uint16_t smid)
{
    return (uint64_t)MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS | ((uint64_t)smid << 16);
}

/* D20b: READ/WRITE killed by our reset is re-sent (outstanding or held again, or done) */
static void CheckResent(Io *io)
{
    CHECK(io->ccb->ccb_h.xnu_resets >= 1);
    CHECK(!io->done || io->status == CAM_REQ_CMP);
}

static void AsyncTests();
static void LoggingTests();
static void ConfigTests();
static void HotplugTests();
static void RetryTests();
static void TemperatureTests();
static void RecoveredSenseTests();
static void CheckConditionLogTests(Rig &g);
static void SleepTests();
static void ReviewTests();
static void DeviationTests();

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);    /* LSan exits with _exit */
    gHost.verbose = getenv("MOCK_VERBOSE") != nullptr;
    if (getenv("MOCK_ONLY")) {              /* one review scenario alone */
        ReviewTests();
        DeviationTests();
        printf("PASS (%s)\n", getenv("MOCK_ONLY"));
        return 0;
    }
    size_t attachMark = LogMark();
    Rig g({0x0009}, nullptr, false);
    gRig = &g;
    MockIocModel &ioc = g.ioc;

    printf("attach\n");
    CHECK(g.attachRc == 0);
    CHECK(ioc.murCount + ioc.diagResetCount >= 1);       /* OPERATIONAL from the boot ROM -> READY */
    CHECK(ioc.state == MPI2_IOC_STATE_OPERATIONAL);
    CHECK(g.sc->num_reqs - g.sc->num_prireqs - 1 == ioc.requestCredit - 1);   /* normal SMIDs */
    /* 128-byte frames: FreeBSD maxio 1 MiB -> 257 segments; 1 SGE + chain in the request frame, 9 + chain per chain frame. */
    CHECK(g.MaxSegs() == kMaxIoPages + 1);
    CHECK(ChainsFor(1) == 0 && ChainsFor(2) == 1 && ChainsFor(10) == 1 &&
          ChainsFor(11) == 2 && ChainsFor(256) == 29 && ChainsFor(257) == 29);
    CHECK(g.ChainsTotal() == ioc.requestCredit * 29 && g.ChainsFree() == g.ChainsTotal());
    CHECK(ioc.him & MPI2_HIM_RIM);                  /* still masked */

    printf("port enable + discovery event, no ack (FreeBSD never sends EVENT_ACK: D29)\n");
    g.Start();
    CHECK(!(ioc.him & MPI2_HIM_RIM));
    CHECK(ioc.portEnables == 1);
    CHECK(g.sc->port_enable_complete == 1);
    CHECK(g.Scans(0) == 1 && g.Present(0, 0x0009));
    CHECK(ioc.ackCount == 0);

    printf("read IO, two segments, fast-path completion\n");
    Buf data = AllocBuf(ioc, 8192);
    const std::vector<MockSeg> seg2 = {{data.iova, 4096}, {data.iova + 4096, 1904}};
    const uint8_t rd10[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 12, 0};
    Io *r1 = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, seg2);
    CHECK(g.Go(r1) == kPosted);
    Io *r1b = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, seg2);
    CHECK(g.Go(r1b) == kPosted && r1b->smid != r1->smid);   /* each IO its own SMID */
    g.Pump();
    CHECK(r1->done == 1 && r1->status == CAM_REQ_CMP && r1b->status == CAM_REQ_CMP);
    CHECK(r1->resid == 0 && r1->scsiStatus == 0);
    CHECK(data.cpu[5999] == Pat(5999, 0));
    CHECK(ioc.lastChains == 1);                     /* FreeBSD: 1 SGE + chain in the request frame */

    printf("no-data IO, CHECK CONDITION with autosense\n");
    const uint8_t tur6[6] = {0x00, 0, 0, 0, 0, 0};
    Io *t1 = g.NewIo(0, 0, CAM_DIR_NONE, tur6, 6, {});
    CHECK(g.Go(t1) == kPosted);
    g.Pump();
    CHECK(t1->done == 1 && t1->status == CAM_SCSI_STATUS_ERROR && t1->scsiStatus == 0x02);
    CHECK(t1->senseLen == 18 && t1->sense[0] == 0x70 && t1->sense[12] == 0x04);

    printf("argument checks\n");
    {
        std::vector<MockSeg> many(g.MaxSegs() + 1, MockSeg{data.iova, 16});
        Io *bad = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, many);
        CHECK(g.Go(bad) == kDone && bad->status == CAM_REQ_TOO_BIG);   /* more than MaxSegs (EFBIG) */
        many.pop_back();
        bad = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, many);
        CHECK(g.Go(bad) == kPosted);                                   /* exactly MaxSegs is fine */
        g.Pump();
        CHECK(bad->status == CAM_REQ_CMP && g.ChainsFree() == g.ChainsTotal());
        bad = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, {{data.iova, 4096}, {data.iova + 4096, 1}});
        bad->ccb->csio.dxfer_len = 6000;
        CHECK(g.Go(bad) == kDone && bad->status == CAM_REQ_INVALID);   /* segments do not add up */
        bad = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, seg2);
        bad->ccb->csio.data_ptr = nullptr;
        CHECK(g.Go(bad) == kDone && bad->status == CAM_REQ_INVALID);   /* no segment array */
        bad = g.NewIo(0, 0, CAM_DIR_NONE, rd10, 10, seg2);
        CHECK(g.Go(bad) == kDone && bad->status == CAM_REQ_INVALID);   /* segments without a data phase */
        bad = g.NewIo(40, 0, CAM_DIR_IN, rd10, 10, seg2);
        CHECK(g.Go(bad) == kDone && bad->status == CAM_TID_INVALID);   /* no such target id */
        bad = g.NewIo(0, 0x1000000, CAM_DIR_IN, rd10, 10, seg2);
        CHECK(g.Go(bad) == kDone && bad->status == CAM_LUN_INVALID);   /* LUN not encodable */
        bad = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, seg2);
        bad->ccb->csio.cdb_len = IOCDBLEN + 1;
        CHECK(g.Go(bad) == kDone && bad->status == CAM_REQ_INVALID);   /* CDB longer than cdb_bytes */
        CHECK(g.ChainsFree() == g.ChainsTotal() && g.sc->io_cmds_active == 0);
    }

    printf("queue wrap: 5000 mixed IOs, batches of 8\n");
    {
        int done = 0;
        for (int i = 0; i < 5000; i += 8) {
            std::vector<Io *> batch;
            for (int j = 0; j < 8; j++) {
                Io *io = (i + j) % 3 ? g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, seg2) :
                                       g.NewIo(0, 0, CAM_DIR_NONE, tur6, 6, {});
                CHECK(g.Go(io) == kPosted);
                batch.push_back(io);
            }
            g.Pump();
            for (Io *io : batch) done += io->done;
        }
        CHECK(done == 5000);
        CHECK(g.sc->io_cmds_active == 0 && g.HeldCount() == 0);       /* nothing left behind */
    }

    printf("chain SGLs: 1, 2, 3, 17, MaxSegs segments, read and write\n");
    uint32_t rng = 0xC0FFEEu;
    Buf big = AllocBuf(ioc, 4u << 20);
    {
        const uint32_t counts[] = {1, 2, 3, 10, 11, 12, 17, 20, 21, g.MaxSegs() - 1, g.MaxSegs()};
        for (uint32_t n : counts) {
            for (uint32_t dir : {CAM_DIR_IN, CAM_DIR_OUT}) {
                std::vector<uint32_t> lens;
                for (uint32_t i = 0; i < n; i++) lens.push_back(1 + Rand(rng) % 8192);
                DataIo(g, dir, Scatter(big, lens, rng), big, (uint8_t)n);
            }
        }
        CHECK(ioc.maxChainsSeen == 29);
    }

    printf("max-size transfers: 1 MiB in 256 x 4 KiB, 3 x max SGE length\n");
    {
        std::vector<uint32_t> lens(kMaxIoPages, 4096);
        for (uint32_t dir : {CAM_DIR_IN, CAM_DIR_OUT})
            DataIo(g, dir, Scatter(big, lens, rng), big, 0x11);
        Buf huge = AllocBuf(ioc, 3ull * kMaxSegBytes + 256);
        std::vector<uint32_t> maxLens(3, kMaxSegBytes);
        for (uint32_t dir : {CAM_DIR_IN, CAM_DIR_OUT})
            DataIo(g, dir, Scatter(huge, maxLens, rng), huge, 0x22);
        Io *tooLong = g.NewIo(0, 0, CAM_DIR_IN, rd10, 10, {{huge.iova, kMaxSegBytes + 1}});
        CHECK(g.Go(tooLong) == kDone && tooLong->status == CAM_REQ_TOO_BIG);   /* one SGE carries at most 16 MiB - 1 */
        FreeBuf(ioc, huge);
    }

    printf("chain frames released after abort, LUN reset and reinit\n");
    {
        std::vector<uint32_t> lens17(17, 512), lensMax(g.MaxSegs(), 64);
        auto s17 = Scatter(big, lens17, rng), sMax = Scatter(big, lensMax, rng);
        Io *a, *b, *c;
        ioc.holdIo = true;
        CHECK(StartData(g, CAM_DIR_IN, s17, big, 1, &a) == kPosted);
        CHECK(StartData(g, CAM_DIR_OUT, sMax, big, 2, &b) == kPosted);
        CHECK(g.ChainsFree() == g.ChainsTotal() - 2 - 29);
        g.Escalate(a);                              /* the OS's abort: a's recovery (D10) */
        CHECK(a->status == CAM_CMD_TIMEOUT);
        CHECK(g.ChainsFree() == g.ChainsTotal() - 29);
        Io *tm = g.ResetDevSync(0);
        CHECK(tm->status == CAM_REQ_CMP && b->status == CAM_REQ_ABORTED);
        CHECK(g.ChainsFree() == g.ChainsTotal());
        /* abort that reaches the IOC after the IO completed: no double release */
        CHECK(StartData(g, CAM_DIR_IN, s17, big, 3, &a) == kPosted);
        ioc.ReleasePending();
        g.Pump();
        CHECK(a->status == CAM_REQ_CMP && g.ChainsFree() == g.ChainsTotal());
        const uint32_t stale = g.sc->xnu.stats.stale_replies;
        ioc.PostRaw(Fast(a->smid));                                 /* duplicate */
        g.Pump();
        CHECK(g.ChainsFree() == g.ChainsTotal() && g.sc->xnu.stats.stale_replies == stale + 1);
        /* reinit with chained IOs outstanding */
        CHECK(StartData(g, CAM_DIR_IN, s17, big, 4, &a) == kPosted);
        CHECK(StartData(g, CAM_DIR_OUT, sMax, big, 5, &b) == kPosted);
        CHECK(StartData(g, CAM_DIR_IN, sMax, big, 6, &c) == kPosted);
        CHECK(g.ChainsFree() == g.ChainsTotal() - 2 - 29 - 29);
        const uint32_t reinits = g.Stats().reinits;
        CHECK(g.ForceReinit() == 0 && g.Stats().reinits == reinits + 1);
        HostRunPending();                                           /* rediscovery */
        /* killed by our reset: re-sent after it (D20b), the IOC holds them again */
        CHECK(!a->done && !b->done && !c->done);
        CHECK(g.IoOutstanding(a) && g.IoOutstanding(b) && g.IoOutstanding(c));
        CHECK(g.ChainsFree() == g.ChainsTotal() - 2 - 29 - 29 && g.ChainsTotal() == ioc.requestCredit * 29);
        CHECK(g.MaxSegs() == kMaxIoPages + 1);
        ioc.holdIo = false;
        ioc.ReleasePending();
        g.Pump();
        CHECK(a->status == CAM_REQ_CMP && b->status == CAM_REQ_CMP && c->status == CAM_REQ_CMP);
        CHECK(g.ChainsFree() == g.ChainsTotal());
        DataIo(g, CAM_DIR_IN, sMax, big, 7);
    }

    printf("random fuzz: 3000 IOs, random segment counts/lengths, up to 8 in flight\n");
    {
        Buf bufs[8];
        for (auto &b : bufs) b = AllocBuf(ioc, 4u << 20);
        uint32_t frng = 0x5EED1234u;
        int ios = 0;
        uint32_t maxSegsUsed = 0;
        const uint64_t checked = ioc.sglChecked;
        while (ios < 3000) {
            uint32_t batch = 1 + Rand(frng) % 8;
            std::vector<std::vector<MockSeg>> segs(batch);
            std::vector<uint32_t> dirs(batch);
            std::vector<Io *> io(batch);
            for (uint32_t j = 0; j < batch; j++) {
                uint32_t n = (Rand(frng) & 1) ? 1 + Rand(frng) % g.MaxSegs() : 1 + Rand(frng) % 24;
                if (n > maxSegsUsed) maxSegsUsed = n;
                std::vector<uint32_t> lens;
                for (uint32_t i = 0; i < n; i++)
                    lens.push_back((Rand(frng) % 16) ? 1 + Rand(frng) % 4096 : 1 + Rand(frng) % 12288);
                segs[j] = Scatter(bufs[j], lens, frng);
                dirs[j] = (Rand(frng) & 1) ? CAM_DIR_IN : CAM_DIR_OUT;
                CHECK(StartData(g, dirs[j], segs[j], bufs[j], (uint8_t)(ios + j), &io[j]) == kPosted);
            }
            g.Pump();
            for (uint32_t j = 0; j < batch; j++) {
                CHECK(io[j]->done == 1 && io[j]->status == CAM_REQ_CMP);
                if (dirs[j] == CAM_DIR_IN) CheckRead(segs[j], bufs[j], (uint8_t)(ios + j));
            }
            CHECK(g.ChainsFree() == g.ChainsTotal());
            ios += (int)batch;
        }
        CHECK(ioc.sglChecked - checked == (uint64_t)ios);
        CHECK(maxSegsUsed > 200);
        printf("    fuzz: %d IOs, max segs %u, chain low-water %u of %u\n",
               ios, maxSegsUsed, g.Stats().chain_free_low, g.ChainsTotal());
        for (auto &b : bufs) FreeBuf(ioc, b);
    }

    const uint32_t kReinits0 = g.Stats().reinits;  /* the chain tests above did one */
    uint8_t lun0[8], lun3[8];
    CHECK(mpssas_set_lun(lun0, 0) == 0 && mpssas_set_lun(lun3, 3) == 0);
    auto ioAt = [&](uint64_t lun) { return g.NewIo(0, lun, CAM_DIR_IN, rd10, 10, seg2); };

    printf("IO to LUN 3\n");
    {
        uint8_t l[8];
        CHECK(mpssas_set_lun(l, 300) == 0 && l[0] == 0x41 && l[1] == 0x2C);
        CHECK(mpssas_set_lun(l, 0x1000000) != 0);
        Io *io = ioAt(3);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->done == 1 && io->status == CAM_REQ_CMP);
        CHECK(ioc.lastLun[0] == 0 && ioc.lastLun[1] == 3 && ioc.lastLun[2] == 0);
    }

    printf("abort of an outstanding IO (high-priority TM)\n");
    {
        ioc.holdIo = true;
        Io *r = ioAt(0);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done == 0 && g.IoOutstanding(r));
        g.Escalate(r);
        CHECK(!g.IoOutstanding(r) && r->done == 1);
        CHECK(r->status == CAM_CMD_TIMEOUT);
        const auto &rec = ioc.tms.back();
        CHECK(rec.type == kAbortT && rec.mid == r->smid && rec.dev == 0x0009);
        CHECK(rec.descFlags == MPI2_REQ_DESCRIPT_FLAGS_HIGH_PRIORITY);
        CHECK(g.Timeout(r) == ENOENT);                               /* not outstanding */
        CHECK(g.TmsBusy() == 0);
    }

    printf("target reset (XPT_RESET_DEV)\n");
    {
        Io *a = ioAt(0), *b = ioAt(0), *c = ioAt(3);
        CHECK(g.Go(a) == kPosted && g.Go(b) == kPosted && g.Go(c) == kPosted);
        CHECK(g.Outstanding(0x0009) == 3 && g.Outstanding(0x0009, 3) == 1);
        Io *tm = g.ResetDevSync(0);
        CHECK(tm->status == CAM_REQ_CMP);
        CHECK(ioc.tms.back().type == kTgtT && ioc.tms.back().msgFlags == MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET);
        CHECK(g.Outstanding(0x0009) == 0);
        CHECK(a->status == CAM_REQ_ABORTED && b->status == CAM_REQ_ABORTED && c->status == CAM_REQ_ABORTED);
        CHECK(g.Stats().reinits == kReinits0);
    }

    printf("timeout escalation: abort and LUN reset ineffective -> target reset\n");
    {
        ioc.tmNoTermMask = (1u << kAbortT) | (1u << kLunT);
        Io *io = ioAt(0);
        CHECK(g.Go(io) == kPosted);
        size_t n = ioc.tms.size();
        g.Escalate(io);
        CHECK(ioc.tms.size() == n + 3 && ioc.tms[n].type == kAbortT &&
              ioc.tms[n + 1].type == kLunT && ioc.tms[n + 2].type == kTgtT);
        CHECK(!g.IoOutstanding(io) && g.Stats().reinits == kReinits0);
        CHECK(g.Stats().recovered_target_reset == 1 && !g.Recovering(0x0009) && !g.ReinitQueued());
        CHECK(io->status == CAM_CMD_TIMEOUT);
        CHECK(g.Timeout(io) == ENOENT);                              /* nothing to do */
    }

    printf("timeout escalation: every TM ineffective -> IOC reinit\n");
    {
        ioc.tmNoTermMask = (1u << kAbortT) | (1u << kLunT) | (1u << kTgtT);
        Io *io = ioAt(0), *by = ioAt(3);                             /* bystander */
        CHECK(g.Go(io) == kPosted && g.Go(by) == kPosted);
        int diag = ioc.diagResetCount, rs = g.AsyncCount(AC_BUS_RESET), pe = ioc.portEnables;
        int scans = g.Scans(0);
        g.Escalate(io);
        CHECK(g.ReinitQueued() && ioc.diagResetCount == diag);      /* decided, not done: nothing blocked */
        CHECK(g.RunReinit() == 0);
        CHECK(ioc.diagResetCount == diag + 1 && g.AsyncCount(AC_BUS_RESET) == rs + 1 && ioc.portEnables == pe + 1);
        CheckResent(io);
        CheckResent(by);
        CHECK(g.Scans(0) == scans + 1 && g.sc->port_enable_complete && g.Present(0, 0x0009));
        CHECK(!(ioc.him & MPI2_HIM_RIM));                            /* interrupts restored */
        CHECK(ioc.state == MPI2_IOC_STATE_OPERATIONAL && g.Stats().reinits == kReinits0 + 1);
        ioc.tmNoTermMask = 0;
    }

    printf("TM timeout -> straight to reinit\n");
    {
        ioc.tmIgnoreMask = 1u << kAbortT;
        Io *io = ioAt(0);
        CHECK(g.Go(io) == kPosted);
        size_t n = ioc.tms.size();
        g.Escalate(io);
        CHECK(!g.ReinitQueued());                                    /* waiting for the TM */
        ioc.nowUs += 5 * 1000000ull + 1000;
        g.Callouts();                                                /* mpssas_tm_timeout */
        CHECK(g.RunReinit() == 0);
        CHECK(ioc.tms.size() == n + 1);                              /* no LUN/target reset */
        CHECK(g.Stats().tm_timeouts == 1 && g.Stats().reinits == kReinits0 + 2);
        CheckResent(io);
        ioc.ReleasePending();                                        /* the re-sent IO */
        g.Pump();
        CHECK(io->done && io->status == CAM_REQ_CMP);
        ioc.tmIgnoreMask = 0;
    }

    printf("lost completion -> escalation ends in reinit\n");
    {
        ioc.holdIo = false; ioc.dropCompletion = true;
        Io *io = ioAt(0);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->done == 0 && g.IoOutstanding(io));
        ioc.dropCompletion = false;
        g.Escalate(io);
        CHECK(g.RunReinit() == 0);
        CheckResent(io);
        Io *again = ioAt(0);
        CHECK(g.Go(again) == kPosted);                               /* and the path works again */
        g.Pump();
        CHECK(again->status == CAM_REQ_CMP);
    }

    printf("IOC FAULT mid-IO -> detected by mps_periodic, reinit, IO resumes\n");
    {
        ioc.holdIo = true;
        Io *a = ioAt(0), *b = ioAt(3);
        CHECK(g.Go(a) == kPosted && g.Go(b) == kPosted);
        ioc.Fault(0x5854);
        const uint32_t faults = g.Stats().faults;
        ioc.nowUs += 1100000;
        g.Callouts();                                                /* mps_periodic's doorbell check (D30) */
        CHECK(g.Faulted() && g.Stats().faults == faults + 1);
        Io *c = ioAt(0);
        CHECK(g.Go(c) == kHeld);                                     /* held until the reinit is done */
        CHECK(c->ccb->ccb_h.xnu_held && g.HeldCount() == 1);
        CHECK(g.RunReinit() == 0 && !g.Faulted());
        CheckResent(a);
        CheckResent(b);
        CHECK(g.IoOutstanding(c) && g.HeldCount() == 0);             /* posted after the reinit */
        ioc.holdIo = false;
        ioc.ReleasePending();
        g.Pump();
        CHECK(c->status == CAM_REQ_CMP);
        CHECK(c->resid == 0);
    }

    printf("IOC FAULT during a TM -> reinit\n");
    {
        ioc.holdIo = true; ioc.faultOnTm = true;
        Io *io = ioAt(0);
        CHECK(g.Go(io) == kPosted);
        g.Escalate(io);
        ioc.nowUs += 1100000;
        g.Callouts();                                                /* nothing answered: mps_periodic (D30) */
        CHECK(g.Faulted() && g.ReinitQueued());
        CHECK(g.RunReinit() == 0);
        CheckResent(io);
        ioc.holdIo = false; ioc.faultOnTm = false;
        ioc.ReleasePending();                                        /* the re-sent IO */
        g.Pump();
        CHECK(io->done && io->status == CAM_REQ_CMP);
        Io *again = ioAt(0);
        CHECK(g.Go(again) == kPosted);
        g.Pump();
        CHECK(again->status == CAM_REQ_CMP);
    }

    printf("malformed / garbage reply descriptors are ignored\n");
    {
        uint32_t frame = ioc.FreeFrameAddr(0);
        const uint16_t idle = 40, tmIdle = 1;
        CHECK(g.sc->commands[idle].cm_state == MPS_CM_STATE_FREE);
        auto addr = [](uint16_t smid, uint32_t a) {
            return (uint64_t)MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY | ((uint64_t)smid << 16) | ((uint64_t)a << 32);
        };
        const uint32_t bad = g.sc->xnu.stats.bad_replies, stale = g.sc->xnu.stats.stale_replies;
        ioc.PostRaw(0x0123456789AB000Eull);                          /* undefined type 0xE */
        ioc.PostRaw(addr(idle, 0x00001234));                         /* below the reply pool */
        ioc.PostRaw(addr(idle, 0xFFFFFF00));                         /* above it */
        ioc.PostRaw(addr(idle, frame + 4));                          /* not frame-aligned */
        ioc.PostRaw(Fast(0xFFFF));                                   /* SMID out of range */
        ioc.PostRaw(Fast(idle + 20));                                /* idle IO SMID */
        ioc.PostRaw(Fast(tmIdle));                                   /* no TM outstanding */
        ioc.PostRaw(Fast(idle));                                     /* no command */
        ioc.PostRaw(Fast(0));                                        /* SMID 0 */
        g.Pump();
        CHECK(!g.ReinitQueued() && !g.Gone());
        CHECK(g.sc->xnu.stats.bad_replies == bad + 5 && g.sc->xnu.stats.stale_replies == stale + 3);
        Io *io = ioAt(0);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->done == 1 && io->status == CAM_REQ_CMP);
    }

    printf("reinit failure (diag reset never completes) -> controller disabled\n");
    {
        Rig g2({0x0009});
        Buf d2 = AllocBuf(g2.ioc, 8192);
        Io *r = g2.NewIo(0, 0, CAM_DIR_IN, rd10, 10, {{d2.iova, 4096}, {d2.iova + 4096, 1904}});
        g2.ioc.holdIo = true;
        CHECK(g2.Go(r) == kPosted);
        g2.ioc.diagStuck = true;
        CHECK(g2.ForceReinit() != 0 && g2.Gone());
        CHECK(g2.Stats().reinit_failures == 1 && g2.ioc.diagResetCount == 1 + 1);   /* one attempt (D55b) */
        /* requeued once RESET_ADAPTER was written (D20b), failed with the dead controller */
        CHECK(r->ccb->ccb_h.xnu_resets == 1 && r->status == CAM_DEV_NOT_THERE);
        Io *r2 = g2.NewIo(0, 0, CAM_DIR_IN, rd10, 10, {{d2.iova, 4096}});
        CHECK(g2.Go(r2) == kDone && r2->status == CAM_DEV_NOT_THERE);
        FreeBuf(g2.ioc, d2);
        g2.Detach();
        CHECK(g2.ioc.liveAllocs == 0);
    }

    printf("IOCFacts limits: frame size and MaxChainDepth\n");
    {
        /* FreeBSD: maxio = ((frame/12 - 1) * depth + 1) pages, segments = maxio/4K + 1, XNU: <= (frame/12 - 1) * depth + 1 */
        struct Cfg { uint16_t dwords; uint8_t depth; uint32_t segs; } cfgs[] = {
            {32, 0, 1},          /* no chaining: the request frame's one SGE */
            {32, 1, 10},         /* 1 + 9 */
            {32, 3, 28},         /* 1 + 9 + 9 + 9 */
            {64, 2, 41},         /* 256-byte frames: 1 + 20 + 20 (the request frame's SGL stays 32 bytes) */
            {16, 128, 0},        /* 64-byte frame cannot hold the SCSI IO request */
        };
        for (const Cfg &cf : cfgs) {
            Rig m({0x0009}, [&](Rig &r) { r.ioc.reqFrameDwords = cf.dwords; r.ioc.maxChainDepth = cf.depth; });
            if (cf.segs == 0) {
                CHECK(m.attachRc != 0);
                m.Detach();
                CHECK(m.ioc.liveAllocs == 0);
                continue;
            }
            CHECK(m.attachRc == 0 && m.MaxSegs() == cf.segs);
            Buf b = AllocBuf(m.ioc, 1u << 20);
            std::vector<uint32_t> lens(cf.segs, 1000);
            DataIo(m, CAM_DIR_IN, Scatter(b, lens, rng), b, 9);
            CHECK(m.ioc.lastChains == ChainsFor(cf.segs, cf.dwords * 4) && m.ioc.lastChains <= cf.depth);
            DataIo(m, CAM_DIR_OUT, Scatter(b, lens, rng), b, 9);
            lens.push_back(1000);
            std::vector<MockSeg> over = Scatter(b, lens, rng);
            Io *r = m.NewIo(0, 0, CAM_DIR_IN, rd10, 10, over);
            CHECK(m.Go(r) == kDone && r->status == CAM_REQ_TOO_BIG);
            FreeBuf(m.ioc, b);
            m.Detach();
            CHECK(m.ioc.liveAllocs == 0);
        }
    }

    printf("chain pool exhaustion: requeued (SIM queue frozen), nothing consumed, recovers\n");
    {
        Rig m({0x0009}, [](Rig &r) { r.sc->max_chains = 5; });     /* hw.mps.max_chains */
        CHECK(m.ChainsTotal() == 5 && m.MaxSegs() == kMaxIoPages + 1);   /* FreeBSD keeps maxio */
        Buf b = AllocBuf(m.ioc, 1u << 20);
        std::vector<uint32_t> l17(17, 256), l11(11, 256), l1(1, 256), l46(46, 256);
        auto s17 = Scatter(b, l17, rng), s11 = Scatter(b, l11, rng), s1 = Scatter(b, l1, rng),
             s46 = Scatter(b, l46, rng);
        Io *a, *bb, *c, *d, *e, *f;
        m.ioc.holdIo = true;
        CHECK(StartData(m, CAM_DIR_IN, s17, b, 1, &a) == kPosted);    /* 2 chains */
        CHECK(StartData(m, CAM_DIR_OUT, s17, b, 2, &bb) == kPosted);  /* 2 chains */
        CHECK(m.ChainsFree() == 1);
        const uint64_t posted = m.ioc.sglChecked;
        CHECK(StartData(m, CAM_DIR_IN, s17, b, 3, &c) == kHeld);      /* chain allocation fails */
        CHECK(StartData(m, CAM_DIR_IN, s1, b, 4, &d) == kHeld);       /* no chain needed, but the SIM queue is frozen */
        CHECK(m.ioc.sglChecked == posted && m.ChainsFree() == 1 && !m.IoOutstanding(c));
        CHECK(m.Stats().chain_alloc_fail == 1 && (m.sc->sassc->flags & MPSSAS_QUEUE_FROZEN));
        CHECK(m.Stats().chain_free_low == 0);                         /* c took the last one, then gave it back */
        m.ioc.ReleasePending();
        m.Pump();                                                     /* a, b done; c, d sent on CAM_RELEASE_SIMQ */
        CHECK(a->status == CAM_REQ_CMP && bb->status == CAM_REQ_CMP);
        CHECK(m.IoOutstanding(c) && m.IoOutstanding(d) && m.ChainsFree() == 3 && m.HeldCount() == 0);
        m.ioc.holdIo = false;
        m.ioc.ReleasePending();
        m.Pump();
        CHECK(c->status == CAM_REQ_CMP && d->status == CAM_REQ_CMP && m.ChainsFree() == 5);
        DataIo(m, CAM_DIR_IN, s46, b, 6);                             /* fits: all 5 */
        CHECK(m.ioc.lastChains == 5);
        DataIo(m, CAM_DIR_IN, s17, b, 7);
        DataIo(m, CAM_DIR_IN, s11, b, 8);
        /* a busy chain pool across a reinit: the reset returns everything */
        m.ioc.holdIo = true;
        CHECK(StartData(m, CAM_DIR_IN, s46, b, 8, &e) == kPosted);
        CHECK(StartData(m, CAM_DIR_IN, s17, b, 8, &f) == kHeld);
        m.ioc.holdIo = false;
        CHECK(m.ForceReinit() == 0 && m.ChainsFree() == 5 && m.ChainsTotal() == 5);
        HostRunPending();                                             /* rediscovery, f sent */
        m.Pump();
        CHECK(e->status == CAM_REQ_CMP && e->ccb->ccb_h.xnu_resets == 1 && f->status == CAM_REQ_CMP);
        DataIo(m, CAM_DIR_IN, s17, b, 9);
        FreeBuf(m.ioc, b);
        m.Detach();
        CHECK(m.ioc.liveAllocs == 0);
    }

    printf("surprise removal with a chained IO outstanding\n");
    Io *chained, *left;
    {
        std::vector<uint32_t> lens(40, 300);
        ioc.holdIo = true;
        CHECK(StartData(g, CAM_DIR_OUT, Scatter(big, lens, rng), big, 1, &chained) == kPosted);
        CHECK(g.ChainsFree() == g.ChainsTotal() - (int)ChainsFor(40));
    }
    left = ioAt(0);
    CHECK(g.Go(left) == kPosted);                    /* left outstanding */
    ioc.dead = true;
    g.Interrupt();                                   /* reads all-ones */
    CHECK(!g.Gone());                                /* D11: confirmed first, from a callout */
    for (int i = 0; i < 150 && !g.Gone(); i++) {
        ioc.nowUs += 1000;
        g.Callouts();
    }
    CHECK(g.Gone());
    Io *tur = g.NewIo(0, 0, CAM_DIR_NONE, tur6, 6, {});
    CHECK(g.Go(tur) == kDone && tur->status == CAM_DEV_NOT_THERE);
    {
        CHECK(chained->done == 0 && left->done == 0);
        HostRunPending();                            /* reinit task: controller disabled */
        CHECK(chained->done == 1 && left->done == 1);
        CHECK(chained->status == CAM_DEV_NOT_THERE);
        CHECK(left->status == CAM_DEV_NOT_THERE);
        CHECK(g.ChainsFree() == g.ChainsTotal());
        CHECK(g.ForceReinit() == ENXIO);
        CHECK(chained->done == 1 && left->done == 1);   /* nothing twice */
    }

    printf("detach\n");
    int mur = ioc.murCount;
    FreeBuf(ioc, data);
    FreeBuf(ioc, big);
    g.Detach();
    CHECK(ioc.murCount == mur);                     /* no register pokes at a dead device */
    CHECK(ioc.liveAllocs == 0);
    CHECK(mps_compat_dmamem_count() == 0 && gHost.armed.empty() && gHost.locksHeld == 0);
    CHECK(HasLog(attachMark, "Firmware: 20.00.00.00"));
    gRig = nullptr;

    AsyncTests();
    LoggingTests();
    ConfigTests();
    HotplugTests();
    RetryTests();
    TemperatureTests();
    RecoveredSenseTests();
    SleepTests();
    ReviewTests();
    DeviationTests();
    printf("PASS (%llu SGLs walked, %zu log lines checked)\n", (unsigned long long)ioc.sglChecked, gHost.log.size());
    return 0;
}

/* ======================================================================
 * M4: asynchronous recovery
 * ==================================================================== */

static void AsyncTests()
{
    printf("async: abort on one target while the other target's IO keeps flowing\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdDev = 0x0009; m.tmDeferMask = 1u << kAbortT;
        Io *f0 = g.Read(0x0009, 0, 7), *f1 = g.Read(0x0009, 0, 8);
        CHECK(g.Go(f0) == kPosted && g.Go(f1) == kPosted);
        m.nowUs += 30000000;
        const int sleeps = gHost.sleeps;
        CHECK(g.Timeout(f0) == 0);
        CHECK(g.Recovering(0x0009) && !g.Recovering(0x000A) && g.TmsBusy() == 1);
        for (int i = 0; i < 40; i++) {               /* target 1 is not affected */
            Io *io = g.Read(0x000A, 0, (uint8_t)(10 + i % 4));
            CHECK(g.Go(io) == kPosted);
            g.Pump();
            CHECK(io->status == CAM_REQ_CMP);
        }
        Io *f2 = g.Read(0x0009, 0, 9);
        CHECK(g.Go(f2) == kHeld);                                    /* target 0 in recovery: held */
        CHECK(g.Outstanding(0x0009) == 2 && g.HeldCount() == 1);
        CHECK(gHost.sleeps == sleeps);                               /* nothing waited */
        m.holdDev = 0;
        m.ReleaseTm();                                               /* the abort is answered */
        g.Pump();
        CHECK(f0->status == CAM_CMD_TIMEOUT);
        CHECK(!g.Recovering(0x0009) && g.Stats().recovered_abort == 1);
        CHECK(f2->status == CAM_REQ_CMP);                            /* held, posted, completed */
        CHECK(g.IoOutstanding(f1));                                  /* not timed out: left alone */
        m.ReleasePending();
        g.Pump();
        CHECK(f1->status == CAM_REQ_CMP && f1->done == 1);
        const struct mps_xnu_stats &st = g.Stats();
        CHECK(st.tm_sent[kAbortT] == 1 && st.aborts == 1 && st.recovered_abort == 1 && st.recoveries == 1);
        CHECK(st.io_timeouts == 1 && st.held == 1 && st.reinits == 0 && st.err_terminated == 1);
        char want[160];
        snprintf(want, sizeof(want), "SMID %u Command timeout on target 0(0x0009) 0 set, 30.", f0->smid);
        CHECK(HasLog(mark, want));
        snprintf(want, sizeof(want), "Sending abort to target 0 for SMID %u", f0->smid);
        CHECK(HasLog(mark, want));
        CHECK(HasLog(mark, "ABORT_TASK SMID 1 to target 0 (handle 0x0009) completed in 1 ms: IOCStatus success (0x0000) "
                           "response task mgmt request completed (0x00) terminated 1"));
        snprintf(want, sizeof(want), "abort TaskMID %u status 0x0 code 0x0 count 1", f0->smid);
        CHECK(HasLog(mark, want));
        CHECK(HasLog(mark, "Finished abort recovery for target 0"));
        CHECK(HasLog(mark, "Unfreezing devq for target ID 0"));
    }

    printf("async: escalation to reinit without blocking; IO arriving during the reinit\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdDev = 0x0009;
        m.tmNoTermMask = (1u << kAbortT) | (1u << kLunT) | (1u << kTgtT);
        Io *f = g.Read(0x0009, 0, 7);
        CHECK(g.Go(f) == kPosted);
        const int sleeps = gHost.sleeps;
        const size_t ntm = m.tms.size();
        g.Escalate(f);
        CHECK(gHost.sleeps == sleeps);                               /* abort, LUN and target reset: no waiting */
        CHECK(m.tms.size() == ntm + 3 && m.tms[ntm].type == kAbortT && m.tms[ntm + 1].type == kLunT &&
              m.tms[ntm + 2].type == kTgtT && m.tms[ntm + 2].msgFlags == MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET);
        CHECK(g.ReinitQueued() && !g.InReset() && g.IoOutstanding(f));
        Io *f5 = g.Read(0x000A, 0, 12);
        CHECK(g.Go(f5) == kHeld);                                    /* reinit pending: held */
        Io *tmr = g.ResetDev(1);
        CHECK(tmr->ccb->ccb_h.xnu_held);                                 /* XPT_RESET_DEV: device-queued, as CAM */
        int during = 0;
        Io *f6 = nullptr, *tmd = nullptr;
        gHost.onOtherThreads = [&] {                                 /* other threads while the reinit sleeps */
            if (!g.InReset() || during++) return;
            f6 = g.Read(0x000A, 0, 13);
            CHECK(g.Go(f6) == kHeld);
            g.Interrupt();                                           /* ignored */
            tmd = g.ResetDev(1);
            CHECK(tmd->ccb->ccb_h.xnu_held);
            CHECK(g.Timeout(f5) == 0);                               /* held: completed at once */
            CHECK(g.ForceReinit() == 0);                             /* "Reset already in progress" */
        };
        m.holdDev = 0;
        CHECK(g.RunReinit() == 0);
        gHost.onOtherThreads = nullptr;
        CHECK(during > 0 && f6 != nullptr);
        CHECK(f->status == CAM_REQ_CMP && f->ccb->ccb_h.xnu_resets == 1);   /* re-sent after the reset (D20b) */
        CHECK(f5->status == CAM_CMD_TIMEOUT);
        CHECK(f6->done == 1 && f6->status == CAM_REQ_CMP && g.HeldCount() == 0);   /* sent after the reinit */
        CHECK(m.portEnables == 2);
        g.Pump();
        for (Io *io : {f, f5, f6, tmr, tmd}) CHECK(io->done == 1);
        CHECK(tmr->status == CAM_REQ_CMP && tmd->status == CAM_REQ_CMP);   /* sent after the reinit */
        const struct mps_xnu_stats &st = g.Stats();
        CHECK(st.reinits == 1 && st.held_timeouts == 1 && st.aborts == 1 && st.lun_resets == 1 && st.target_resets == 1);
        CHECK(st.recovered_abort + st.recovered_lun_reset + st.recovered_target_reset == 0);
        CHECK(HasLog(mark, "Abort failed for target 0, sending logical unit reset"));
        CHECK(HasLog(mark, "logical unit reset complete for target 0, but still have 1 command(s), sending target reset"));
        CHECK(HasLog(mark, "Target reset complete for target 0, but still have 1 command(s), resetting controller"));
        CHECK(HasLog(mark, "controller reinit requested: mpssas_target_reset_complete"));
        CHECK(HasLog(mark, "Reinitializing controller"));
        CHECK(HasLog(mark, "Reset already in progress"));
        CHECK(HasLog(mark, "held IO for target 1 lun 0 timed out before it was sent"));
        CHECK(HasLog(mark, "during recovery") && HasLog(mark, "completed timedout cm"));
        CHECK(HasLog(mark, "reinit done in"));
        CHECK(HasLog(mark, "mpssas_startup_decrement releasing simq"));
    }

    printf("async: TM timeout, then the late reply and duplicates\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdDev = 0x0009; m.tmDeferMask = 1u << kAbortT;
        Io *f = g.Read(0x0009, 0, 7);
        CHECK(g.Go(f) == kPosted);
        CHECK(g.Timeout(f) == 0);
        CHECK(g.Timeout(f) == 0 && g.Stats().io_timeouts == 1);       /* second timer: already recovering */
        m.nowUs += 4700000;
        g.Callouts();
        struct mps_command *tm = g.Targ(0x0009)->tm;
        CHECK(!g.ReinitQueued() && tm != nullptr && callout_pending(&tm->cm_callout));
        const uint64_t left = tm->cm_callout.c_time_us - m.nowUs;
        CHECK(left <= 300000 && left >= 299000);                     /* TM deadline: 5 s after the abort */
        m.nowUs += 400000;
        g.Callouts();                                                /* mpssas_tm_timeout */
        CHECK(g.ReinitQueued() && g.Stats().tm_timeouts == 1 && g.TmsBusy() == 1);
        CHECK(HasLog(mark, "timed out") && HasLog(mark, "controller reinit requested: mpssas_tm_timeout"));
        m.ReleaseTm();                                               /* firmware answers after all */
        g.Pump();
        CHECK(g.Stats().tm_late_replies == 1 && g.TmsBusy() == 1 && g.ReinitQueued());   /* the reinit frees it */
        CHECK(HasLog(mark, "late reply for timed-out task management SMID 1 ignored (reinit pending)"));
        CHECK(f->status == CAM_CMD_TIMEOUT);
        const uint32_t stale = g.Stats().stale_replies;
        m.PostRaw(Fast(1));                                          /* the TM again */
        m.PostRaw(Fast(f->smid));                                    /* the IO again */
        g.Pump();
        CHECK(g.Stats().stale_replies == stale + 1 && g.Stats().tm_late_replies == 2 && f->done == 1);
        char want[96];
        snprintf(want, sizeof(want), "stale reply for idle SMID %u (state 0), ignored", f->smid);
        CHECK(HasLog(mark, want));
        m.holdDev = 0;
        CHECK(g.RunReinit() == 0 && !g.ReinitQueued() && g.TmsBusy() == 0);
        Io *again = g.Read(0x0009, 0, 7);
        CHECK(g.Go(again) == kPosted);
        g.Pump();
        CHECK(again->status == CAM_REQ_CMP);
    }

    printf("async: OS target reset times out -> reinit completes it\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        m.tmIgnoreMask = 1u << kTgtT;
        Io *t41 = g.ResetDev(0), *t42 = g.ResetDev(1);
        CHECK(t41->done == 0 && t42->done == 0);
        CHECK(g.TmsBusy() == 2);
        m.nowUs += 30 * 1000000ull;
        g.Callouts();
        CHECK(t41->done == 0 && g.ReinitQueued());                   /* mpssas_tm_timeout: the IOC owns it */
        CHECK(g.RunReinit() == 0 && g.TmsBusy() == 0);                /* lost TMs freed by the reset */
        CHECK(t41->done == 1 && t42->done == 1 && t41->status == CAM_REQ_ABORTED && t42->status == CAM_REQ_ABORTED);
        m.tmIgnoreMask = 0; m.tmDeferMask = 1u << kTgtT;
        Io *t43 = g.ResetDev(1);
        m.ReleaseTm(); g.Pump();
        CHECK(t43->done == 1 && t43->status == CAM_REQ_CMP);
        /* a TM in flight when a reinit happens: CAM_REQ_ABORTED */
        Io *t44 = g.ResetDev(1);
        m.Fault(0x2100);
        m.nowUs += 1100000;
        g.Callouts();                                                /* mps_periodic */
        CHECK(g.RunReinit() == 0 && t44->done == 1 && t44->status == CAM_REQ_ABORTED);
    }

    printf("async: more timed-out targets than TM SMIDs\n");
    {
        /* 4 TMs (the pool before the review fixes), so 6 targets must wait */
        Rig g({0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E},
              [](Rig &r) { r.sc->max_prireqframes = 4; r.ioc.tmSmidHi = 1 + 4; });
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdIo = true; m.tmDeferMask = 1u << kAbortT;
        std::vector<Io *> io;
        for (uint16_t i = 0; i < 6; i++) {
            io.push_back(g.Read((uint16_t)(0x0009 + i), 0, (uint8_t)i));
            CHECK(g.Go(io.back()) == kPosted);
            CHECK(g.Timeout(io.back()) == 0);
        }
        CHECK(g.TmsBusy() == 4 && m.tms.size() == 4);
        CHECK(HasLog(mark, "failed to allocate a tm"));
        for (int round = 0; round < 3 && g.Stats().recovered_abort < 6; round++) {
            m.ReleaseTm();
            g.Pump();
        }
        CHECK(g.Stats().recovered_abort == 6 && g.Stats().recoveries == 6 && m.tms.size() == 6);
        CHECK(HasLog(mark, "(TM freed)"));
        for (Io *x : io) CHECK(x->status == CAM_CMD_TIMEOUT);
        m.holdIo = false;
    }

    printf("async: IOC fault during the reinit (nested fault) -> one attempt (D55b), disabled\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdIo = true;
        Io *f = g.Read(0x0009, 0, 7);
        CHECK(g.Go(f) == kPosted);
        m.Fault(0x5854);
        m.nowUs += 1100000;
        g.Callouts();                                                /* mps_periodic */
        CHECK(g.Faulted() && g.ReinitQueued() && g.Stats().faults == 1);
        CHECK(HasLog(mark, "IOC Fault 0x40005854, Resetting"));
        CHECK(HasLog(mark, "controller reinit requested: mps_periodic"));
        m.faultOnIocInitOnce = true; m.holdIo = false;
        const int diag = m.diagResetCount;
        CHECK(g.RunReinit() != 0);
        CHECK(m.diagResetCount == diag + 1 && g.Stats().reinit_failures == 1 && g.Stats().reinits == 1);
        CHECK(!HasLog(mark, "retrying") && HasLog(mark, "controller disabled"));
        g.Pump();
        CHECK(f->status == CAM_DEV_NOT_THERE && f->done == 1);      /* completed once */
        CHECK(g.Gone() && g.sc->xnu.disabled && g.HeldCount() == 0);
    }

    printf("async: surprise removal in the middle of a recovery\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdIo = true; m.tmDeferMask = (1u << kAbortT) | (1u << kTgtT);
        Io *f0 = g.Read(0x0009, 0, 7), *f1 = g.Read(0x0009, 0, 8);
        CHECK(g.Go(f0) == kPosted && g.Go(f1) == kPosted);
        CHECK(g.Timeout(f0) == 0);
        Io *f2 = g.Read(0x0009, 0, 9);
        CHECK(g.Go(f2) == kHeld);
        Io *t55 = g.ResetDev(1);
        CHECK(t55->done == 0);
        const int mur = m.murCount;
        Io *late = g.Read(0x0009, 0, 7);                             /* built now: a reinit may clear the handles */
        m.dead = true;
        HostAdvance(1300);                                           /* mps_periodic reads all-ones (D11: confirmed) */
        CHECK(g.Gone() && !g.ReinitQueued());
        CHECK(HasLog(mark, "controller gone (surprise removal)") && HasLog(mark, "controller disabled"));
        CHECK(g.ForceReinit() == ENXIO);                             /* no register access, nothing twice */
        for (Io *io : {f0, f1, f2}) CHECK(io->status == CAM_DEV_NOT_THERE && io->done == 1);
        CHECK(t55->done == 1 && t55->status == CAM_DEV_NOT_THERE);
        CHECK(g.TmsBusy() == 0 && g.HeldCount() == 0);
        CHECK(g.Go(late) == kDone && late->status == CAM_DEV_NOT_THERE);
        g.Detach();
        CHECK(m.murCount == mur);                                    /* nothing written to a dead device */
    }

    printf("async: surprise removal while the reinit thread sleeps\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdIo = true;
        Io *f = g.Read(0x0009, 0, 7);
        CHECK(g.Go(f) == kPosted);
        m.Fault(0x0101);
        m.nowUs += 1100000;
        g.Callouts();
        Io *f1 = g.Read(0x000A, 0, 8);
        CHECK(g.Go(f1) == kHeld);
        const int sleeps = gHost.sleeps;
        gHost.onOtherThreads = [&] { m.dead = true; };
        CHECK(g.RunReinit() == ENXIO && g.Gone());
        gHost.onOtherThreads = nullptr;
        CHECK(gHost.sleeps - sleeps < 10);                           /* gave up at once */
        CHECK(f->status == CAM_DEV_NOT_THERE);
        CHECK(f1->status == CAM_DEV_NOT_THERE);
        CHECK(HasLog(mark, "controller gone (surprise removal)"));
        CHECK(HasLog(mark, "controller disabled"));
    }

    printf("async: detach while the reinit thread sleeps (abandoned) and with TMs in flight\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdIo = true;
        Io *f = g.Read(0x0009, 0, 7);
        CHECK(g.Go(f) == kPosted);
        m.Fault(0x0202);
        m.nowUs += 1100000;
        g.Callouts();
        Io *f1 = g.Read(0x000A, 0, 8);
        CHECK(g.Go(f1) == kHeld);
        const int sleeps = gHost.sleeps;
        gHost.onOtherThreads = [&] {                                 /* the owner's stop path (mps_free) */
            if (g.InReset()) g.sc->mps_flags |= MPS_FLAGS_SHUTDOWN;
        };
        CHECK(g.RunReinit() == ENXIO && g.Gone());
        gHost.onOtherThreads = nullptr;
        CHECK(gHost.sleeps - sleeps <= 2);
        CHECK(HasLog(mark, "controller disabled"));
        CHECK(f->done == 1 && f1->done == 1);
        CHECK(f->status == CAM_DEV_NOT_THERE && f1->status == CAM_DEV_NOT_THERE);
    }
    {
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdIo = true; m.tmDeferMask = (1u << kAbortT) | (1u << kTgtT);
        Io *f = g.Read(0x0009, 0, 7);
        CHECK(g.Go(f) == kPosted);
        CHECK(g.Timeout(f) == 0);
        Io *f1 = g.Read(0x0009, 0, 8);
        CHECK(g.Go(f1) == kHeld);
        Io *t66 = g.ResetDev(1);
        const int mur = m.murCount, freed = gHost.interruptsFreed;
        g.Detach();                                                  /* mps_free */
        CHECK(m.murCount == mur + 1);                                /* IOC stopped before buffers are returned */
        CHECK(f->status == CAM_DEV_NOT_THERE && f1->status == CAM_DEV_NOT_THERE);
        CHECK(t66->status == CAM_DEV_NOT_THERE);
        CHECK(gHost.interruptsFreed == freed + 1);                   /* no completion can follow */
        CHECK(f->done == 1 && f1->done == 1 && t66->done == 1);
    }
}

/* ======================================================================
 * M4: logging
 * ==================================================================== */

static void LoggingTests()
{
    printf("logging: debug classes, level presets, mps_parse_debug\n");
    {
        Rig g({0x0009});
        struct mps_softc *sc = g.sc;
        CHECK(mps_xnu_level_mask(MPS_LEVEL_ERROR) == (MPS_FAULT | MPS_ERROR));
        CHECK(mps_xnu_level_mask(MPS_LEVEL_WARN) == (MPS_FAULT | MPS_ERROR | MPS_RECOVERY));
        const uint32_t info = mps_xnu_level_mask(MPS_LEVEL_INFO);
        CHECK(info == (MPS_INFO | MPS_FAULT));                               /* FreeBSD default (D22) */
        const uint32_t dbg = mps_xnu_level_mask(MPS_LEVEL_DEBUG);
        CHECK((dbg & MPS_RECOVERY) && (dbg & MPS_EVENT) && (dbg & MPS_INIT) && (dbg & MPS_XINFO) && !(dbg & MPS_TRACE));
        CHECK(mps_xnu_level_mask(MPS_LEVEL_TRACE) == MPS_DEBUG_ALL && mps_xnu_level_mask(99) == MPS_DEBUG_ALL);
        auto parse = [&](const char *s) { std::string t(s); mps_parse_debug(sc, &t[0]); return sc->mps_debug; };
        sc->mps_debug = MPS_INFO | MPS_FAULT;
        CHECK(parse("+recovery") == (MPS_INFO | MPS_FAULT | MPS_RECOVERY));
        CHECK(parse("-info") == (MPS_FAULT | MPS_RECOVERY));
        CHECK(parse("0x13") == 0x13);
        CHECK(parse("Info:fault,trace") == (MPS_INFO | MPS_FAULT | MPS_TRACE));
        CHECK(parse("+bogus") == (MPS_INFO | MPS_FAULT | MPS_TRACE));   /* unknown names add nothing */
        CHECK(parse("") == (MPS_INFO | MPS_FAULT | MPS_TRACE) && parse("+") == (MPS_INFO | MPS_FAULT | MPS_TRACE));
        CHECK(parse("bogus") == 0);                                     /* FreeBSD: a plain list replaces the mask */
        CHECK(mps_xnu_parse_level("debug") == 3 && mps_xnu_parse_level("4") == 4 && mps_xnu_parse_level("WARN") == 1);
        CHECK(mps_xnu_parse_level("x") == -1 && mps_xnu_parse_level("5") == -1 && mps_xnu_parse_level(nullptr) == -1);
        CHECK(strcmp(mps_xnu_category(MPS_RECOVERY | MPS_INFO), "recovery") == 0);
        CHECK(strcmp(mps_xnu_category(MPS_INFO), "info") == 0 && strcmp(mps_xnu_category(MPS_FAULT | MPS_INIT), "fault") == 0);
        char b[96];
        mps_xnu_describe_debug(MPS_INFO | MPS_FAULT | MPS_TRACE, b, sizeof(b));
        CHECK(strcmp(b, "info,fault,trace") == 0);
        sc->mps_debug = MPS_DEBUG_ALL;
    }

    printf("logging: mps_table.c decoders\n");
    {
        CHECK(strcmp(mps_describe_table(mps_event_names, 0x1c), "SasTopologyChangeList") == 0);
        CHECK(strcmp(mps_describe_table(mps_event_names, 0x99), "Unknown Event") == 0);
        CHECK(strcmp(mps_describe_table(mps_iocstatus_string, 0x8048 & MPI2_IOCSTATUS_MASK), "scsi task terminated") == 0);
        CHECK(strcmp(mps_describe_table(mps_iocstatus_string, 0x7777), "unknown") == 0);
        CHECK(strcmp(mps_describe_table(mps_linkrate_names, 0xA), "6.0Gbps") == 0 &&
              strcmp(mps_describe_table(mps_linkrate_names, 0x7), "LinkRate Unknown") == 0);
        CHECK(strcmp(mps_describe_table(mps_scsi_taskmgmt_string, 0), "task mgmt request completed") == 0);
        CHECK(strcmp(mps_xnu_ioc_state_str(0x40002622), "FAULT") == 0 && strcmp(mps_xnu_ioc_state_str(0x70000000), "UNKNOWN") == 0);
        CHECK(strcmp(mps_describe_table(mps_scsi_status_string, 0x18), "reservation conflict") == 0);
        char b[200];
        mps_describe_devinfo(0x401, b, sizeof(b));          /* FreeBSD's %b: no 0x */
        CHECK(strcmp(b, "401<SspTarg>,End Device") == 0);
        mps_xnu_loginfo_str(0x31120303, b, sizeof(b));
        CHECK(strcmp(b, "0x31120303 (SAS PL code 0x12 sub 0x0303)") == 0);
        const uint8_t fixed[18] = {0x70, 0, 0x03, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x11, 0x00};
        const uint8_t desc[8] = {0x72, 0x06, 0x29, 0x00};
        uint8_t k, a, q;
        CHECK(mps_xnu_parse_sense(fixed, 18, &k, &a, &q) && k == 3 && a == 0x11 && q == 0);
        CHECK(mps_xnu_parse_sense(desc, 8, &k, &a, &q) && k == 6 && a == 0x29 && q == 0);
        CHECK(!mps_xnu_parse_sense(fixed, 10, &k, &a, &q));
        CHECK(strcmp(mps_xnu_asc_str(0x04, 0x99), "LOGICAL UNIT NOT READY") == 0 && mps_xnu_asc_str(0x77, 0) == nullptr);
        const uint8_t r16[16] = {0x88, 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 1, 0};
        uint64_t lba; uint32_t blocks;
        CHECK(mps_xnu_cdb_lba(r16, 16, &lba, &blocks) && lba == 0x100000002ull && blocks == 256);
        uint8_t lun[8] = {0x41, 0x2C};
        mps_xnu_lun_str(lun, b, sizeof(b));
        CHECK(strcmp(b, "lun 300") == 0);
        struct mps_xnu_ratelimit rl = {};
        uint32_t sup = 0, passed = 0;
        for (int i = 0; i < 10; i++) passed += mps_xnu_ratelimit_allow(&rl, 1000 + i, 3, 100, &sup);
        CHECK(passed == 3 && !mps_xnu_ratelimit_flush(&rl, 1050, 100, &sup) &&
              mps_xnu_ratelimit_flush(&rl, 1200, 100, &sup) && sup == 7);
        CHECK(mps_xnu_ratelimit_allow(&rl, 1201, 3, 100, &sup) && sup == 0);
    }

    printf("logging: attach/discovery lines, IOCFacts dump, events\n");
    {
        const size_t mark = LogMark();
        Rig g;
        CHECK(HasLog(mark, "Firmware: 20.00.00.00, Driver: "));
        CHECK(HasLog(mark, "IOCCapabilities: "));
        CHECK(HasLog(mark, "Sized queues to q=1 reqs=108 replies=127"));
        CHECK(HasLog(mark, "RequestCredit 100 HighPriorityCredit 8 ReplyPostQueueDepth 128: 2900 chain frames"));
        CHECK(HasLog(mark, "FWVersion= 20-0-0-0") && HasLog(mark, "HighPriorityCredit: 8"));
        CHECK(HasLog(mark, "IOCInit"));
        CHECK(HasLog(mark, "reply frames busaddr") && HasLog(mark, "sense frames busaddr"));
        CHECK(HasLog(mark, "SasTopologyChangeList"));
        CHECK(HasLog(mark, "PHY[0].LinkRate: 6.0Gbps (0xa0)") && HasLog(mark, "PHY[1].PhyStatus: NewTargetAdded"));
        CHECK(HasLog(mark, "Target id 0x0 added") || HasLog(mark, "Target id 0x0"));
        CHECK(HasLog(mark, "mpssas_startup_decrement releasing simq"));
        CHECK(!HasLog(mark, "ack event") && CountLog(mark, "no command for the ack") == 0);
    }

    printf("logging: failed-IO decode, rate limit with summary, counters, hot-path filter\n");
    {
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        size_t mark = LogMark();
        m.failDev = 0x0009;
        for (int i = 0; i < 20; i++) {
            Io *io = g.Read(0x0009, 0, 7);
            CHECK(g.Go(io) == kPosted);
            g.Pump();
            CHECK(io->status == CAM_REQ_CMP_ERR && io->senseLen == 0);   /* FreeBSD: no sense for IO_DATA_ERROR */
        }
        CHECK(CountLog(mark, "failed: IOCStatus scsi io data error (0x0046)") == 5);
        CHECK(HasLog(mark, "target 0 (handle 0x0009) lun 0 SMID "));
        CHECK(HasLog(mark, " READ(10) lba 7 blocks 8 failed: IOCStatus scsi io data error "
                           "(0x0046) loginfo 0x31120303 (SAS PL code 0x12 sub 0x0303) SCSI status check condition (0x02) "
                           "state 0x01 sense MEDIUM ERROR (0x3) ASC/ASCQ 0x11/0x00 UNRECOVERED READ ERROR len 4096 IOC TransferCount 0"));
        CHECK(mps_xnu_log_type(LogClass(mark, "READ(10) lba 7 blocks 8 failed")) == MPS_XNU_LOG_ERROR);   /* IOC failure: ERROR */
        CHECK(g.Stats().err_transport == 20 && g.Stats().ios == 20);
        m.nowUs += 11000000;
        g.Callouts();                                                /* mps_periodic flushes the summary */
        CHECK(HasLog(mark, "target 0 (handle 0x0009): 15 similar IO error lines suppressed"));
        m.failDev = 0;

        const uint64_t ios0 = g.Stats().ios, rb0 = g.Stats().read_bytes;
        g.sc->mps_debug = mps_xnu_level_mask(MPS_LEVEL_INFO);
        mark = LogMark();
        for (int i = 0; i < 100; i++) {
            Io *io = g.Read(0x0009, 0, (uint8_t)(i % 8));
            CHECK(g.Go(io) == kPosted);
            g.Pump();
        }
        CHECK(LogMark() == mark);                                    /* no lines for good IO at info */
        const struct mps_xnu_stats &st = g.Stats();
        CHECK(st.ios == ios0 + 100 && st.read_bytes == rb0 + 100 * 4096 && st.write_bytes == 0);
        CHECK(st.lat_max_us >= 37 && st.lat_sum_us >= 37 * st.ios);
        g.sc->mps_debug = mps_xnu_level_mask(MPS_LEVEL_TRACE);
        Io *io = g.Read(0x0009, 0, 7);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        char want[160];
        snprintf(want, sizeof(want), "IO start SMID %u target 0 (handle 0x0009) lun 0 READ(10) lba 7 blocks 8 "
                 "len 4096 segs 1 chains 0", io->smid);
        CHECK(HasLog(mark, want));
        snprintf(want, sizeof(want), "IO done SMID %u cam status 0x1 scsi good xfer 4096/4096 latency 37 us", io->smid);
        CHECK(HasLog(mark, want));
        g.sc->mps_debug = 0;
        mark = LogMark();
        m.Fault(0x1234);
        m.nowUs += 1100000;
        g.Callouts();
        CHECK(LogMark() == mark && g.Faulted());                     /* mask 0: silent, still detected */
        memset(&g.sc->xnu.stats, 0, sizeof(g.sc->xnu.stats));       /* sas2308ctl reset-stats */
        CHECK(g.sc->xnu.stats.ios == 0 && g.sc->xnu.stats.faults == 0);
    }
}

/* ---- SAS Device Page 0 (mps_config.c, mpssas_add_device) ------------------- */

static void ConfigTests()
{
    printf("config: SAS Device Page 0 for each discovered device and its parent (the HBA)\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        CHECK(g.Present(g.Tid(0x0009), 0x0009) && g.Present(g.Tid(0x000A), 0x000A));
        CHECK(m.configHeaders == 4 && m.configReads == 4);       /* device + parent, header + page each */
        CHECK(m.configChained == 4);                             /* FreeBSD: the SGE goes in a chain frame */
        for (uint16_t h : {0x0009, 0x000A}) {
            struct mpssas_target *t = g.Targ(h);
            CHECK(t->sasaddr == MockIocModel::SasAddrOf(h));
            CHECK(t->parent_sasaddr == MockIocModel::kHostSasAddr);
            CHECK(t->devinfo == (MPI2_SAS_DEVICE_INFO_SSP_TARGET | MPI2_SAS_DEVICE_INFO_END_DEVICE));
            CHECK(t->parent_handle == MockIocModel::kHostHandle);
            CHECK(t->devname == ((0x5000C500ull << 32) | (0xABC00000u + h)));
            uint64_t a = 0; uint32_t di = 0; uint8_t lr = 0;
            mps_lock(g.sc);
            CHECK(mps_xnu_target_info(g.sc, t->tid, &a, &di, &lr) == 1);
            mps_unlock(g.sc);
            CHECK(a == MockIocModel::SasAddrOf(h) && di == t->devinfo && lr == 0xA);   /* 6.0 Gbit/s */
        }
        uint64_t a = 0; uint32_t di = 0; uint8_t lr = 0;
        CHECK(mps_xnu_target_info(g.sc, 40, &a, &di, &lr) == 0);
        CHECK(g.Stats().config_reads == 4 && g.Stats().config_failures == 0);
        CHECK(g.ChainsFree() == g.ChainsTotal() && g.sc->io_cmds_active == 0);
        CHECK(mps_compat_bufmap_count() == 0);                          /* every page unmapped (D6) */

        printf("config: hot-added device gets its page read from the event task\n");
        size_t mark = LogMark();
        m.devices.push_back(0x000B);
        m.PostTopologyEvent({0x000B});
        HostRunPending();
        CHECK(g.Present(g.Tid(0x000B), 0x000B));
        CHECK(g.Targ(0x000B)->sasaddr == MockIocModel::SasAddrOf(0x000B));
        CHECK(m.lastConfigHandle == MockIocModel::kHostHandle && m.configReads == 6);
        CHECK(HasLog(mark, "Handle 0x000b SAS Address from SAS device page0 = 5000c5001234000b"));
        Io *io = g.Read(0x000B);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->status == CAM_REQ_CMP);
    }

    printf("config: page read fails -> that device is not added (FreeBSD)\n");
    {
        size_t mark = LogMark();
        Rig g({0x0009, 0x000A}, [](Rig &r) { r.ioc.configFailHandle = 0x000A; });
        CHECK(g.Targ(0x0009) != nullptr && g.Targ(0x000A) == nullptr);
        CHECK(g.Stats().config_failures == 1);
        CHECK(HasLog(mark, "Error reading SAS device 0xa page0, iocstatus= 0x22"));
        CHECK(HasLog(mark, "page read with error; iocstatus = 0x22"));
        CHECK(mps_compat_bufmap_count() == 0 && g.ChainsFree() == g.ChainsTotal());
    }

    printf("config: parent page read fails -> device added without a parent address\n");
    {
        size_t mark = LogMark();
        Rig g({0x0009}, [](Rig &r) { r.ioc.configFailHandle = MockIocModel::kHostHandle; });
        CHECK(g.Targ(0x0009) != nullptr && g.Targ(0x0009)->parent_sasaddr == 0);
        CHECK(g.Targ(0x0009)->sasaddr == MockIocModel::SasAddrOf(0x0009));
        CHECK(HasLog(mark, "Error reading parent SAS device 0x1 page0"));
        CHECK(g.Stats().config_failures == 0);
    }

    printf("config: an IOC that loses the reply -> mps_wait_command timeout -> reinit, device found again\n");
    {
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        const uint32_t reinits = g.Stats().reinits;
        m.devices.push_back(0x000B);
        m.dropConfigOnce = true;
        m.PostTopologyEvent({0x000B});
        HostRunPending();                        /* waits 60 s (virtual), mps_reinit, rediscovery */
        CHECK(g.Stats().reinits == reinits + 1);
        CHECK(g.Present(g.Tid(0x0009), 0x0009) && g.Present(g.Tid(0x000B), 0x000B));
        CHECK(mps_compat_bufmap_count() == 0 && g.ChainsFree() == g.ChainsTotal());
    }
}

/* ---- hot-plug of several drives at once (hardware, 2026-11-20 02:36) -------- */

/* `n' READ(10)s to each of `handles', held by the IOC (in flight). */
static std::vector<Io *> HoldIo(Rig &g, const std::vector<uint16_t> &handles, int n)
{
    std::vector<Io *> v;
    g.ioc.holdIo = true;
    for (uint16_t h : handles)
        for (int i = 0; i < n; i++) {
            Io *io = g.Read(h, 0, (uint8_t)(v.size() + 1));
            CHECK(g.Go(io) == kPosted);
            v.push_back(io);
        }
    return v;
}

static void ReleaseIo(Rig &g, const std::vector<Io *> &v)
{
    g.ioc.holdIo = false;
    g.ioc.ReleasePending();
    g.Pump();
    for (Io *io : v)
        CHECK(io->done == 1 && io->status == CAM_REQ_CMP);
}

static void HotplugTests()
{
    /*
     * The SFF-8087 breakout on port 0 (PHYs 0, 2, 3) is replugged while the
     * drives on port 1 are busy.  The IT firmware posts one topology change
     * list per PHY; the next one arrives while the event task sleeps in the
     * previous device's page 0 read.  On xnu the event taskqueue's thread
     * call then ran a second (third) mpssas_firmware_event_work concurrently:
     * the single config page buffer was busy, page0 failed with no firmware
     * reply (iocstatus 0) and the drive was never added.
     */
    printf("hotplug: 3 drives on one port, one topology list per PHY, IO in flight to the others\n");
    {
        size_t mark = LogMark();
        const std::vector<uint16_t> port1 = {0x000A, 0x000D, 0x000E, 0x000F};
        Rig g(port1);
        MockIocModel &m = g.ioc;
        for (uint16_t h : port1) CHECK(g.Targ(h) != nullptr);
        std::vector<Io *> busy = HoldIo(g, port1, 4);
        const int reentries = gHost.queueReentries;
        const uint32_t reads = g.Stats().config_reads;
        for (uint16_t h : {0x0009, 0x000B, 0x000C}) m.devices.push_back(h);
        m.topologyOnConfig = {{0x000B}, {0x000C}};
        m.PostTopologyEvent({0x0009});
        HostRunPending();
        CHECK(m.topologyOnConfig.empty());
        CHECK(gHost.queueReentries > reentries);            /* the thread call did re-enter */
        for (uint16_t h : {0x0009, 0x000B, 0x000C}) {
            CHECK(g.Targ(h) != nullptr && g.Present(g.Tid(h), h));
            CHECK(g.Targ(h)->sasaddr == MockIocModel::SasAddrOf(h));
        }
        CHECK(g.Stats().config_failures == 0 && g.Stats().config_reads == reads + 6);
        CHECK(!HasLog(mark, "Error reading SAS device"));
        CHECK(mps_compat_bufmap_count() == 0);
        ReleaseIo(g, busy);

        printf("hotplug: the same port unplugged and replugged as one 3-entry list while port 1 changes\n");
        mark = LogMark();
        busy = HoldIo(g, port1, 2);
        m.PostTopologyEvent({0x0009, 0x000B, 0x000C}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
        HostRunPending();
        g.Pump();
        HostRunPending();
        for (uint16_t h : {0x0009, 0x000B, 0x000C}) CHECK(g.Targ(h) == nullptr);
        for (uint16_t h : {0x0009, 0x000B, 0x000C, 0x0010}) m.devices.push_back(h);
        m.topologyOnConfig = {{0x0010}};
        m.PostTopologyEvent({0x0009, 0x000B, 0x000C});
        HostRunPending();
        for (uint16_t h : {0x0009, 0x000B, 0x000C, 0x0010})
            CHECK(g.Targ(h) != nullptr && g.Present(g.Tid(h), h));
        CHECK(g.Stats().config_failures == 0);
        CHECK(!HasLog(mark, "Error reading SAS device"));
        ReleaseIo(g, busy);
        CHECK(g.ChainsFree() == g.ChainsTotal() && mps_compat_bufmap_count() == 0);
    }

    printf("hotplug: 7 drives arrive together at port enable (one list per PHY)\n");
    {
        const std::vector<uint16_t> all = {0x0009, 0x000B, 0x000C, 0x000A, 0x000D, 0x000E, 0x000F};
        Rig g(all, [](Rig &r) {
            /* port enable posts the first list; the rest come one per page read */
            r.ioc.portEnableList = {0x0009};
            r.ioc.topologyOnConfig = {{0x000B}, {0x000C}, {0x000A}, {0x000D}, {0x000E}, {0x000F}};
        });
        CHECK(g.ioc.topologyOnConfig.empty());
        for (uint16_t h : all) CHECK(g.Targ(h) != nullptr && g.Present(g.Tid(h), h));
        CHECK(g.Stats().config_failures == 0 && g.Stats().config_reads == 14);
    }
}

/* Every normal command taken (the pool exhausted, as by IO plus event acks). */
static std::vector<struct mps_command *> TakeCommands(Rig &g)
{
    std::vector<struct mps_command *> v;
    mps_lock(g.sc);
    for (struct mps_command *cm; (cm = mps_alloc_command(g.sc)) != nullptr; )
        v.push_back(cm);
    mps_unlock(g.sc);
    return v;
}

static void GiveCommands(Rig &g, std::vector<struct mps_command *> &v)
{
    mps_lock(g.sc);
    for (struct mps_command *cm : v)
        mps_free_command(g.sc, cm);
    mps_unlock(g.sc);
    v.clear();
}

static int ReaddPending(Rig &g)
{
    int n = 0;
    for (const auto &r : g.sc->xnu.readd) n += r.handle != 0;
    return n;
}

static void RetryTests()
{
    printf("page0 retry: command pool exhausted -> in-line retries, then the add is retried later\n");
    {
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        size_t mark = LogMark();
        std::vector<struct mps_command *> held = TakeCommands(g);
        CHECK(!held.empty());
        m.devices.push_back(0x000B);
        m.PostTopologyEvent({0x000B});
        HostRunPending();
        CHECK(g.Targ(0x000B) == nullptr);
        CHECK(g.Stats().config_retries == MPS_XNU_CFG_TRIES - 1);
        CHECK(g.Stats().readd_scheduled == 1 && ReaddPending(g) == 1);
        CHECK(HasLog(mark, "retry 2/3 in 10 ms") && HasLog(mark, "retry 3/3 in 20 ms"));
        CHECK(HasLog(mark, "handle 0x000b: SAS device page0 read failed (error 16: no free command, "
                           "iocstatus 0x0): add retried in 1000 ms (1/8)"));
        CHECK(LogClass(mark, "add retried in 1000 ms") & MPS_FAULT);
        HostAdvance(2100);                      /* 1 s + the 1 s callout: still no command */
        CHECK(g.Targ(0x000B) == nullptr && g.Stats().readd_scheduled == 2);
        CHECK(HasLog(mark, "adding the device again (1/8)") && HasLog(mark, "add retried in 2000 ms (2/8)"));
        GiveCommands(g, held);
        HostAdvance(3100);
        CHECK(g.Targ(0x000B) != nullptr && g.Present(g.Tid(0x000B), 0x000B));
        CHECK(g.Targ(0x000B)->sasaddr == MockIocModel::SasAddrOf(0x000B));
        CHECK(ReaddPending(g) == 0 && g.Stats().readd_given_up == 0);
        Io *io = g.Read(0x000B);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->status == CAM_REQ_CMP && mps_compat_bufmap_count() == 0);
    }

    printf("page0 retry: device gone before the later add -> cancelled; reinit -> rediscovery adds it\n");
    {
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        size_t mark = LogMark();
        std::vector<struct mps_command *> held = TakeCommands(g);
        m.devices.push_back(0x000B);
        m.PostTopologyEvent({0x000B});
        HostRunPending();
        CHECK(ReaddPending(g) == 1);
        GiveCommands(g, held);
        m.devices.pop_back();
        m.PostTopologyEvent({0x000B}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
        HostRunPending();
        CHECK(ReaddPending(g) == 0);
        HostAdvance(3000);
        CHECK(!HasLog(mark, "adding the device again") && g.Targ(0x000B) == nullptr);

        held = TakeCommands(g);
        m.devices.push_back(0x000C);
        m.PostTopologyEvent({0x000C});
        HostRunPending();
        CHECK(ReaddPending(g) == 1);
        GiveCommands(g, held);
        CHECK(g.ForceReinit() == 0);
        CHECK(ReaddPending(g) == 0);
        HostRunPending();                       /* rediscovery */
        CHECK(g.Targ(0x000C) != nullptr && g.Present(g.Tid(0x000C), 0x000C));
        HostAdvance(3000);
        CHECK(!HasLog(mark, "adding the device again"));
    }

    printf("page0 retry: pool exhausted for good -> retries exhausted, fault line, nothing leaked\n");
    {
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        size_t mark = LogMark();
        std::vector<struct mps_command *> held = TakeCommands(g);
        m.devices.push_back(0x000B);
        m.PostTopologyEvent({0x000B});
        HostRunPending();
        HostAdvance(50000);
        CHECK(g.Stats().readd_scheduled == MPS_XNU_READD_TRIES && g.Stats().readd_given_up == 1);
        CHECK(HasLog(mark, "handle 0x000b: SAS device page0 read failed (error 16: no free command, "
                           "iocstatus 0x0): device not added, retries exhausted"));
        CHECK(ReaddPending(g) == 0 && g.Targ(0x000B) == nullptr);
        GiveCommands(g, held);
        HostAdvance(3000);
        CHECK(g.Targ(0x000B) == nullptr && mps_compat_bufmap_count() == 0);
    }

    printf("page0: firmware error (invalid page) is final, as on FreeBSD: no retry\n");
    {
        size_t mark = LogMark();
        Rig g({0x0009, 0x000A}, [](Rig &r) { r.ioc.configFailHandle = 0x000A; });
        CHECK(g.Targ(0x000A) == nullptr && g.Stats().config_retries == 0);
        CHECK(g.Stats().readd_scheduled == 0 && g.Stats().readd_given_up == 1);
        CHECK(HasLog(mark, "handle 0x000a: SAS device page0 read failed (error 6: no reply or IOC error, "
                           "iocstatus 0x22): device not added"));
    }
}

static void TemperatureTests()
{
    printf("temperature: IO Unit Page 7 read from the event queue every 30 s, published in the statistics\n");
    {
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        size_t mark = LogMark();
        const uint32_t reads = g.Stats().config_reads;
        HostAdvance(1100);
        CHECK(m.iounit7Reads == 1 && m.iounit7Headers == 1);
        CHECK(g.Stats().temp_reads == 1 && g.Stats().temp_failures == 0);
        CHECK(g.Stats().ioc_temp_valid == 1 && g.Stats().ioc_temp_c == 52);
        CHECK(g.Stats().config_reads == reads);              /* page0 count untouched */
        CHECK(HasLog(mark, "IOC temperature 52 C (IO Unit Page 7, polled every 30 s)"));
        CHECK(LogClass(mark, "IOC temperature 52 C (IO Unit") & MPS_INFO);
        HostAdvance(20000);
        CHECK(m.iounit7Reads == 1);
        HostAdvance(11000);
        CHECK(m.iounit7Reads == 2 && gHost.Count(mark, "polled every 30 s") == 1);
        /* the poll is our work item 0xFF02 on the event queue, not an IOC event: xinfo, named */
        CHECK(LogClass(mark, "Working on  Event: [ff02] XnuTemperaturePoll") == MPS_XINFO);
        CHECK(LogClass(mark, "Event Free: [ff02]") == MPS_XINFO);

        printf("temperature: warning at 85 C once, re-armed below 80 C\n");
        mark = LogMark();
        m.iocTemp = 86;
        HostAdvance(30000);
        CHECK(g.Stats().ioc_temp_c == 86 && g.Stats().temp_warnings == 1);
        CHECK(HasLog(mark, "IOC temperature 86 C: at or above 85 C"));
        CHECK(LogClass(mark, "at or above 85 C") & MPS_ERROR);
        m.iocTemp = 90;
        HostAdvance(30000);
        m.iocTemp = 82;                                     /* hysteresis: still armed off */
        HostAdvance(30000);
        CHECK(g.Stats().ioc_temp_c == 82 && g.Stats().temp_warnings == 1);
        CHECK(gHost.Count(mark, "at or above 85 C") == 1 && !HasLog(mark, "below 80 C again"));
        m.iocTemp = 79;
        HostAdvance(30000);
        CHECK(HasLog(mark, "IOC temperature 79 C: below 80 C again"));
        m.iocTemp = 85;
        HostAdvance(30000);
        CHECK(g.Stats().temp_warnings == 2 && gHost.Count(mark, "at or above 85 C") == 2);
        m.iocTemp = 185;                                    /* Fahrenheit: 85 C */
        m.iocTempUnits = MPI2_IOUNITPAGE7_IOC_TEMP_FAHRENHEIT;
        HostAdvance(30000);
        CHECK(g.Stats().ioc_temp_c == 85 && g.Stats().temp_warnings == 2);

        printf("temperature: not read while suspended\n");
        const int before = m.iounit7Reads;
        mps_lock(g.sc);
        CHECK(XSUSPEND(g.sc, 1000) == 0);
        mps_unlock(g.sc);
        HostAdvance(65000);
        CHECK(m.iounit7Reads == before);
        mps_lock(g.sc);
        CHECK(mps_xnu_resume(g.sc) == 0);
        mps_unlock(g.sc);
        HostRunPending();
        HostAdvance(31000);
        CHECK(m.iounit7Reads > before && g.Present(g.Tid(0x0009), 0x0009));
    }

    printf("sizing: FreeBSD defaults (%d + %d frames) bounded by IOCFacts; OS commands = normal frames - %d\n",
           MPS_REQ_FRAMES, MPS_PRI_REQ_FRAMES, 8);   /* the kext keeps 8 (MPSK_INTERNAL_CMDS, D21) */
    {
        struct Case { uint16_t credit, hpc, depth; int replyTune; int prireq, reqcr, io; const char *what; } cases[] = {
            {100, 8, 128, 0, 8, 100, 92, "the model's default"},
            {10240, 8, 65520, 0, 8, 2048, 2040, "credit above 2048: min() = MPS_REQ_FRAMES"},
            {2049, 8, 65520, 0, 8, 2048, 2040, "credit 2049"},
            {2047, 8, 65520, 0, 8, 2047, 2039, "credit 2047: min() = RequestCredit"},
            {1500, 8, 65520, 0, 8, 1500, 1492, "credit below 2048"},
            {10240, 200, 65520, 0, 128, 2048, 2040, "HighPriorityCredit above 128: replies cover 2176 frames"},
            {10240, 8, 65520, 64, 8, 2048, 2040, "max_replyframes tuned to 64: replies still cover every frame"},
            {10240, 8, 1600, 0, 8, 1591, 1583, "post queue 1600: 1600 - 8 - 1 frames"},
            {10240, 128, 2096, 0, 128, 1967, 1959, "post queue 2096, 128 TMs: 2096 - 128 - 1"},
            {1024, 8, 208, 0, 8, 199, 191, "short post queue: 208 - 8 - 1"},
            {200, 8, 1024, 0, 8, 200, 192, "credit 200"},
            {12, 8, 128, 0, 8, 12, 4, "tiny credit"},
        };
        for (const Case &c : cases) {
            printf("  credit %u, high-priority %u, post queue %u: %s\n", c.credit, c.hpc, c.depth, c.what);
            Rig g({0x0009}, [&](Rig &r) {
                r.ioc.requestCredit = c.credit; r.ioc.highPriorityCredit = c.hpc; r.ioc.replyPostDepth = c.depth;
                r.ioc.tmSmidHi = 1 + std::min<int>(MPS_PRI_REQ_FRAMES, c.hpc);
                if (c.replyTune) r.sc->max_replyframes = c.replyTune;
            });
            CHECK(g.attachRc == 0);
            CHECK(g.sc->num_prireqs == c.prireq);
            CHECK(g.sc->num_reqs == c.reqcr + c.prireq);
            CHECK(std::max(1, g.sc->num_reqs - g.sc->num_prireqs - 8) == c.io);   /* the kext's share (D21) */
            CHECK(g.sc->num_reqs - g.sc->num_prireqs - 1 >= c.io);       /* normal SMIDs */
            const int replyFrames = c.replyTune ? c.replyTune : MPS_REPLY_FRAMES;
            CHECK(g.sc->num_replies ==
                  std::min(std::max(replyFrames, g.sc->num_reqs) + MPS_EVT_REPLY_FRAMES, (int)c.depth) - 1);
            /* the reply-queue clamp: a post queue descriptor for every request frame */
            CHECK(g.sc->num_replies >= g.sc->num_reqs - 1 && g.sc->pqdepth - 1 >= g.sc->num_reqs - 1);
            CHECK(g.sc->pqdepth <= c.depth && g.ioc.pq == (uint32_t)g.sc->pqdepth);
            CHECK(g.ChainsTotal() == std::min(29 * c.reqcr, MPS_CHAIN_FRAMES) && g.ChainsFree() == g.ChainsTotal());
            CHECK(g.Present(g.Tid(0x0009), 0x0009));
            /* io_cmds at once, all posted (none held), all complete at once (no post queue overrun) */
            g.ioc.holdIo = true;
            std::vector<Io *> v;
            for (int i = 0; i < c.io; i++) {
                v.push_back(g.Read(0x0009, 0, (uint8_t)i));
                CHECK(g.Go(v.back()) == kPosted);
            }
            CHECK(g.HeldCount() == 0 && (int)g.ioc.pending.size() == c.io);
            CHECK(g.ioc.maxSmid < c.prireq + c.credit && g.ioc.maxSmid < g.sc->num_reqs);
            g.ioc.holdIo = false;
            g.ioc.ReleasePending();
            for (int i = 0; i < 8; i++) g.Pump();
            for (Io *io : v) CHECK(io->done == 1 && io->status == CAM_REQ_CMP);
            CHECK(g.sc->io_cmds_active == 0 && g.ChainsFree() == g.ChainsTotal());
        }
    }

    printf("temperature: a read the IOC never answers -> 5 s, no reinit, polling stops until a reinit (D31)\n");
    {
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        auto busy = [&g]() {
            int n = 0;
            for (int i = 1; i < g.sc->num_reqs; i++)
                n += g.sc->commands[i].cm_state != MPS_CM_STATE_FREE;
            return n;
        };
        HostAdvance(1100);
        CHECK(m.iounit7Reads == 1 && g.Stats().ioc_temp_valid == 1);
        const uint32_t reinits = g.Stats().reinits;
        const int busy0 = busy();
        size_t mark = LogMark();
        m.dropIoUnit7 = true;
        HostAdvance(31000);                                 /* due: the header request is lost */
        CHECK(m.iounit7Dropped == 1);
        HostAdvance(5100);                                  /* 5 s timeout, not 60 */
        CHECK(g.Stats().temp_timeouts == 1 && g.Stats().reinits == reinits && !g.ReinitQueued());
        CHECK(HasLog(mark, "IO Unit Page 7 read not answered in 5 s: temperature polling stopped until the next controller reinit"));
        CHECK(LogClass(mark, "IO Unit Page 7 read not answered") & MPS_ERROR);
        CHECK(busy() == busy0 + 1);                         /* the IOC owns that command: not freed */
        Io *r = g.Read(0x0009, 0, 2);                       /* the event queue is not blocked, IO flows */
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
        HostAdvance(120000);                                /* no more polls, so at most one command leaked */
        CHECK(m.iounit7Dropped == 1 && g.Stats().temp_timeouts == 1 && g.Stats().reinits == reinits);
        CHECK(gHost.Count(mark, "IO Unit Page 7 read not answered") == 1 && busy() == busy0 + 1);
        m.dropIoUnit7 = false;
        mps_lock(g.sc);
        mps_xnu_request_reinit(g.sc, "test");               /* the reinit reclaims the command ... */
        mps_unlock(g.sc);
        CHECK(g.RunReinit() == 0);
        HostRunPending();
        CHECK(busy() == busy0);
        const int reads = m.iounit7Reads;
        HostAdvance(31000);                                 /* ... and polling resumes */
        CHECK(m.iounit7Reads > reads && g.Stats().ioc_temp_valid == 1 && g.Stats().temp_timeouts == 1);
    }

    printf("temperature: no sensor (units NOT_PRESENT) -> logged once, no more polls\n");
    {
        Rig g({0x0009}, [](Rig &r) { r.ioc.iocTempUnits = MPI2_IOUNITPAGE7_IOC_TEMP_NOT_PRESENT; });
        size_t mark = LogMark();
        HostAdvance(1100);
        CHECK(g.ioc.iounit7Reads == 1 && g.Stats().ioc_temp_valid == 0);
        CHECK(HasLog(mark, "IOC temperature: no sensor (IO Unit Page 7 units 0x0), not polled"));
        HostAdvance(95000);
        CHECK(g.ioc.iounit7Reads == 1 && g.Stats().temp_reads == 1);
    }

    printf("temperature: due during a hot-plug page read -> runs after it, no page0 failure\n");
    {
        Rig g({0x000A});
        MockIocModel &m = g.ioc;
        HostAdvance(1100);                                  /* first read done */
        CHECK(m.iounit7Reads == 1);
        /*
         * The 1 s callout fires (30 s after the last read) while the event
         * task sleeps in a page 0 read: first with the next topology list
         * waiting (deferred), then during the last device's read (queued
         * behind it, read after it).
         */
        int fired = 0, lastPage = -1;
        const int base = m.configReads;
        gHost.onOtherThreads = [&]() {
            if (mps_compat_bufmap_count() == 0 || m.configReads == lastPage) return;   /* a page read in flight */
            lastPage = m.configReads;
            if (lastPage != base + 1 && lastPage != base + 3) return;   /* 0x0009, 0x000B pages */
            fired++;
            mps_lock(g.sc);
            g.sc->xnu.temp_last_us = 0;                         /* due */
            mps_xnu_periodic(g.sc);
            CHECK(g.sc->xnu.temp_queued == (fired == 1 ? 0 : 1));   /* events waiting: deferred */
            mps_unlock(g.sc);
            CHECK(m.iounit7Reads == 1);
        };
        for (uint16_t h : {0x0009, 0x000B}) m.devices.push_back(h);
        m.topologyOnConfig = {{0x000B}};
        m.PostTopologyEvent({0x0009});
        HostRunPending();
        gHost.onOtherThreads = nullptr;
        CHECK(fired == 2 && m.iounit7Reads == 2 && g.Stats().temp_failures == 0);
        for (uint16_t h : {0x0009, 0x000B}) CHECK(g.Targ(h) != nullptr && g.Present(g.Tid(h), h));
        CHECK(g.Stats().config_failures == 0 && g.Stats().config_retries == 0 && mps_compat_bufmap_count() == 0);
    }
}

static void RecoveredSenseTests()
{
    printf("ATA PASS-THROUGH(12/16) with RECOVERED ERROR (SAT status return): xinfo line, not an error\n");
    Rig g({0x0009});
    size_t mark = LogMark();
    const struct mps_xnu_stats before = g.Stats();
    for (uint8_t op : {(uint8_t)0xA1, (uint8_t)0x85}) {
        uint8_t cdb[16] = {op};
        Io *io = g.NewIo(g.Tid(0x0009), 0, CAM_DIR_NONE, cdb, op == 0xA1 ? 12 : 16, {});
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->done && io->status == CAM_SCSI_STATUS_ERROR && io->scsiStatus == 0x02);
        CHECK(io->senseLen >= 4 && io->sense[0] == 0x72 && io->sense[1] == 0x01 && io->sense[3] == 0x1D);
    }
    CHECK(g.Stats().recovered_sense == before.recovered_sense + 2);
    CHECK(g.Stats().err_check_condition == before.err_check_condition);
    CHECK(HasLog(mark, "ATA PASS-THROUGH(12) lba 0 blocks 0 recovered: IOCStatus success"));
    CHECK(HasLog(mark, "ASC/ASCQ 0x00/0x1d ATA PASS THROUGH INFORMATION AVAILABLE"));
    CHECK(LogClass(mark, "ATA PASS-THROUGH(12) lba 0 blocks 0 recovered") == MPS_XINFO);
    CHECK(LogClass(mark, "ATA PASS-THROUGH(16) lba 0 blocks 0 recovered") == MPS_XINFO);
    for (size_t i = mark; i < gHost.log.size(); i++) CHECK(!(gHost.log[i].cls & MPS_ERROR));
    g.sc->mps_debug = mps_xnu_level_mask(MPS_LEVEL_INFO);   /* default level: silent */
    mark = LogMark();
    uint8_t cdb[12] = {0xA1};
    Io *io = g.NewIo(g.Tid(0x0009), 0, CAM_DIR_NONE, cdb, 12, {});
    CHECK(g.Go(io) == kPosted);
    g.Pump();
    CHECK(io->done && !HasLog(mark, "ATA PASS-THROUGH"));
    g.sc->mps_debug = MPS_DEBUG_ALL;
    CheckConditionLogTests(g);
}

/*
 * A command completed with a SCSI status (IOCStatus success): xinfo (os_log
 * DEBUG) as FreeBSD mps, except CHECK CONDITION MEDIUM/HARDWARE ERROR (CAM
 * prints that sense): error, rate limited.  IOC failures stay error.
 */
static void CheckConditionLogTests(Rig &g)
{
    printf("check condition: NOT READY 0x04/0x02 on TEST UNIT READY is xinfo; MEDIUM/HARDWARE ERROR stay error\n");
    const struct mps_xnu_stats before = g.Stats();
    size_t mark = LogMark();
    uint8_t tur[6] = {0x00};
    Io *io = g.NewIo(g.Tid(0x0009), 0, CAM_DIR_NONE, tur, 6, {});
    CHECK(g.Go(io) == kPosted);
    g.Pump();
    /* no data phase: DataLength 0, NODATATRANSFER, no SGL data; the family gets 0 bytes */
    CHECK(g.ioc.lastDataLength == 0 &&
          (g.ioc.lastControl & MPI2_SCSIIO_CONTROL_DATADIRECTION_MASK) == MPI2_SCSIIO_CONTROL_NODATATRANSFER);
    CHECK(io->done && io->status == CAM_SCSI_STATUS_ERROR && io->scsiStatus == 0x02 && io->resid == 0);
    CHECK(g.Stats().err_check_condition == before.err_check_condition + 1);
    CHECK(HasLog(mark, "TEST UNIT READY lba 0 blocks 0 failed: IOCStatus success (0x0000) SCSI status check condition "
                       "(0x02) state 0x01 sense NOT READY (0x2) ASC/ASCQ 0x04/0x02 "));
    CHECK(HasLog(mark, "ASC/ASCQ 0x04/0x02 LOGICAL UNIT NOT READY, INITIALIZING COMMAND REQUIRED no data") &&
          !HasLog(mark, "TransferCount"));   /* the reply's TransferCount (65536) is meaningless without data */
    CHECK(LogClass(mark, "TEST UNIT READY lba 0 blocks 0 failed") == MPS_XINFO);
    for (size_t i = mark; i < gHost.log.size(); i++) CHECK(mps_xnu_log_type(gHost.log[i].cls) != MPS_XNU_LOG_ERROR);
    /* a flood (macOS polls every ~3 s per drive): none of it is an error line, none suppressed */
    mark = LogMark();
    for (int i = 0; i < 20; i++) {
        io = g.NewIo(g.Tid(0x0009), 0, CAM_DIR_NONE, tur, 6, {});
        CHECK(g.Go(io) == kPosted);
        g.Pump();
    }
    CHECK(CountLog(mark, "TEST UNIT READY lba 0 blocks 0 failed") == 20 && !HasLog(mark, "suppressed"));
    for (size_t i = mark; i < gHost.log.size(); i++) CHECK(mps_xnu_log_type(gHost.log[i].cls) != MPS_XNU_LOG_ERROR);
    /* default level: silent */
    g.sc->mps_debug = mps_xnu_level_mask(MPS_LEVEL_INFO);
    mark = LogMark();
    io = g.NewIo(g.Tid(0x0009), 0, CAM_DIR_NONE, tur, 6, {});
    CHECK(g.Go(io) == kPosted);
    g.Pump();
    CHECK(io->done && LogMark() == mark);
    CHECK(g.Stats().err_check_condition == before.err_check_condition + 22);

    /* CHECK CONDITION, IOCStatus success: by sense key */
    struct { uint8_t key, asc, ascq; bool error; const char *name; } cases[] = {
        {0x03, 0x11, 0x00, true, "sense MEDIUM ERROR (0x3) ASC/ASCQ 0x11/0x00"},
        {0x04, 0x44, 0x00, true, "sense HARDWARE ERROR (0x4) ASC/ASCQ 0x44/0x00"},
        {0x05, 0x24, 0x00, false, "sense ILLEGAL REQUEST (0x5) ASC/ASCQ 0x24/0x00"},
        {0x06, 0x29, 0x00, false, "sense UNIT ATTENTION (0x6) ASC/ASCQ 0x29/0x00"},
        {0x0B, 0x47, 0x03, false, "sense ABORTED COMMAND (0xb) ASC/ASCQ 0x47/0x03"},
    };
    for (auto &c : cases) {
        g.sc->mps_debug = mps_xnu_level_mask(MPS_LEVEL_INFO);
        g.ioc.ccDev = 0x0009; g.ioc.ccKey = c.key; g.ioc.ccAsc = c.asc; g.ioc.ccAscq = c.ascq;
        g.ioc.nowUs += 11000000;                    /* a fresh rate-limit window */
        mark = LogMark();
        io = g.Read(0x0009, 0, 7);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->done && io->status == CAM_SCSI_STATUS_ERROR && io->scsiStatus == 0x02);
        if (c.error) {
            CHECK(HasLog(mark, c.name) && HasLog(mark, "READ(10) lba 7 blocks 8 failed: IOCStatus success (0x0000)"));
            CHECK(HasLog(mark, " len 4096 IOC TransferCount 0"));
            CHECK(mps_xnu_log_type(LogClass(mark, c.name)) == MPS_XNU_LOG_ERROR);
        } else {
            CHECK(LogMark() == mark);               /* xinfo: nothing at the default level */
            g.sc->mps_debug = MPS_DEBUG_ALL;
            mark = LogMark();
            io = g.Read(0x0009, 0, 7);
            CHECK(g.Go(io) == kPosted);
            g.Pump();
            CHECK(LogClass(mark, c.name) == MPS_XINFO &&
                  mps_xnu_log_type(LogClass(mark, c.name)) == MPS_XNU_LOG_DEBUG);
        }
    }
    g.ioc.ccDev = 0;
    g.sc->mps_debug = MPS_DEBUG_ALL;

    /* an IOC failure is still an ERROR line */
    mark = LogMark();
    g.ioc.nowUs += 11000000;
    g.ioc.failDev = 0x0009;
    io = g.Read(0x0009, 0, 7);
    CHECK(g.Go(io) == kPosted);
    g.Pump();
    g.ioc.failDev = 0;
    CHECK(io->done && io->status == CAM_REQ_CMP_ERR);
    CHECK(mps_xnu_log_type(LogClass(mark, "failed: IOCStatus scsi io data error (0x0046)")) == MPS_XNU_LOG_ERROR);

    /* the class -> os_log type map (the kext's LogType) */
    CHECK(mps_xnu_log_type(MPS_ERROR) == MPS_XNU_LOG_ERROR && mps_xnu_log_type(MPS_FAULT) == MPS_XNU_LOG_FAULT);   /* D24 */
    CHECK(mps_xnu_log_type(MPS_RECOVERY) == MPS_XNU_LOG_DEFAULT && mps_xnu_log_type(0) == MPS_XNU_LOG_DEFAULT);
    CHECK(mps_xnu_log_type(MPS_INIT | MPS_INFO) == MPS_XNU_LOG_DEFAULT);
    CHECK(mps_xnu_log_type(MPS_XINFO) == MPS_XNU_LOG_DEBUG && mps_xnu_log_type(MPS_TRACE) == MPS_XNU_LOG_DEBUG);
}

/* ---- system sleep and shutdown (mps_xnu_suspend / mps_xnu_resume) ------------ */

static int Suspend(Rig &g, u_int drainMs)
{
    mps_lock(g.sc);
    int rc = XSUSPEND(g.sc, drainMs);
    mps_unlock(g.sc);
    return rc;
}

static int Resume(Rig &g)
{
    mps_lock(g.sc);
    int rc = mps_xnu_resume(g.sc);
    mps_unlock(g.sc);
    HostRunPending();                           /* the reinit task, rediscovery */
    return rc;
}

static void SleepTests()
{
    printf("sleep: idle suspend -> MUR, interrupts masked, no register access; resume -> reinit, same targets\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        const target_id_t t9 = g.Tid(0x0009), tA = g.Tid(0x000A);
        const int murs = m.murCount, resets = m.diagResetCount, pe = m.portEnables;
        CHECK(Suspend(g, 1000) == 0);
        CHECK(m.murCount == murs + 1 && m.state == MPI2_IOC_STATE_READY);
        CHECK(m.him & MPI2_HIM_RIM);
        CHECK(g.sc->mps_flags & MPS_FLAGS_XNU_SUSPENDED);
        CHECK(HasLog(mark, "suspending: 0 commands outstanding") && HasLog(mark, "suspended: IOC stopped"));
        CHECK(Suspend(g, 1000) == 0 && m.murCount == murs + 1);        /* twice: no-op */
        HostAdvance(5000);                                            /* periodic stopped: no doorbell reads */
        CHECK(g.Stats().suspended_access == 0 && !HasLog(mark, "accessed while suspended"));
        CHECK(g.Present(t9, 0x0009) && g.Present(tA, 0x000A));       /* targets kept while asleep */
        CHECK(Resume(g) == 0);
        CHECK(!(g.sc->mps_flags & (MPS_FLAGS_XNU_SUSPENDED | MPS_FLAGS_DIAGRESET)) && !g.ReinitQueued());
        CHECK(m.diagResetCount == resets && m.portEnables == pe + 1);       /* READY: FreeBSD attach sequence (D12) */
        CHECK(m.state == MPI2_IOC_STATE_OPERATIONAL && !(m.him & MPI2_HIM_RIM));
        CHECK(g.Tid(0x0009) == t9 && g.Tid(0x000A) == tA);           /* same ids after the rediscovery */
        CHECK(g.Stats().suspends == 1 && g.Stats().resumes == 1 && g.Stats().suspend_drain_left == 0);
        CHECK(Resume(g) == 0 && g.Stats().resumes == 1);               /* not suspended: no-op */
        Io *io = g.Read(0x0009);
        CHECK(g.Go(io) == kPosted);
        g.Pump();
        CHECK(io->status == CAM_REQ_CMP);
        HostAdvance(3000);                                            /* periodic running again */
        CHECK(!g.Gone() && g.Stats().suspended_access == 0);
    }

    printf("sleep: IO completing during the drain completes normally\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdIo = true;
        Io *a = g.Read(0x0009, 0, 1), *b = g.Read(0x000A, 0, 2);
        CHECK(g.Go(a) == kPosted && g.Go(b) == kPosted);
        const uint64_t t0 = m.nowUs;
        gHost.onOtherThreads = [&m, t0]() { if (m.nowUs > t0 + 50000 && !m.pending.empty()) m.ReleasePending(); };
        CHECK(Suspend(g, 2000) == 0);
        gHost.onOtherThreads = nullptr;
        CHECK(a->done && a->status == CAM_REQ_CMP && b->done && b->status == CAM_REQ_CMP);
        CHECK(g.Stats().suspend_drain_left == 0 && g.sc->io_cmds_active == 0);
        m.holdIo = false;
        CHECK(Resume(g) == 0);
    }

    printf("sleep: IO still in flight after the drain -> MUR, requeued (held), re-sent after wake\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        m.holdIo = true;
        Io *a = g.Read(0x0009, 0, 3);
        CHECK(g.Go(a) == kPosted);
        const uint64_t t0 = m.nowUs;
        CHECK(Suspend(g, 200) == 0);
        CHECK(m.nowUs - t0 >= 200000 && m.nowUs - t0 < 1000000);     /* waited the drain time, not more */
        CHECK(!a->done && a->ccb->ccb_h.xnu_held && a->ccb->ccb_h.xnu_resets == 1);   /* requeued (D20b) */
        CHECK(m.pending.empty() && g.ChainsFree() == g.ChainsTotal());
        CHECK(g.Stats().suspend_drain_left == 1);
        CHECK(HasLog(mark, "suspend: 1 commands still outstanding after 200 ms, stopping the IOC"));
        m.holdIo = false;
        /* new IO while asleep: held, not posted */
        const int posted = (int)m.sglChecked;
        Io *r = g.Read(0x0009, 0, 3), *n = g.Read(0x000A, 0, 4), *t = g.Read(0x000A, 0, 5);
        CHECK(g.Go(r) == kHeld && g.Go(n) == kHeld && g.Go(t) == kHeld && g.HeldCount() == 4);
        CHECK(g.Timeout(t) == 0 && t->done && t->status == CAM_CMD_TIMEOUT && g.HeldCount() == 3);
        Io *tm = g.ResetDev(g.Tid(0x000A));
        CHECK(tm->ccb->ccb_h.xnu_held && g.HeldCount() == 4);               /* no TMs while asleep: held */
        g.Pump(); HostAdvance(2000);
        CHECK(!r->done && !n->done && (int)m.sglChecked == posted && g.Stats().suspended_access == 0);
        CHECK(Resume(g) == 0);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP && n->done && n->status == CAM_REQ_CMP);
        CHECK(a->done && a->status == CAM_REQ_CMP);
        CHECK(tm->done && tm->status == CAM_REQ_CMP);
        CHECK(g.HeldCount() == 0 && g.sc->io_cmds_active == 0);
    }

    printf("sleep: a reinit pending at suspend finishes first\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const uint32_t reinits = g.Stats().reinits;
        m.Fault(0x2100);
        m.nowUs += 1100000;
        g.Callouts();
        CHECK(g.Faulted());
        CHECK(Suspend(g, 5000) == 0);
        CHECK(g.Stats().reinits == reinits + 1 && g.Stats().suspend_drain_left == 0);
        CHECK(m.state == MPI2_IOC_STATE_READY && !g.Gone());
        CHECK(Resume(g) == 0 && g.Stats().reinits == reinits + 2);
        CHECK(g.Present(g.Tid(0x0009), 0x0009));
    }

    printf("sleep: TM in flight at suspend completes (aborted), recovery state cleared\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdDev = 0x0009; m.tmIgnoreMask = 1u << kAbortT;
        Io *a = g.Read(0x0009, 0, 6);
        CHECK(g.Go(a) == kPosted);
        CHECK(g.Timeout(a) == 0);                                    /* its recovery: an ABORT_TASK */
        CHECK(g.TmsBusy() == 1);
        CHECK(Suspend(g, 100) == 0);
        CHECK(!a->done && a->ccb->ccb_h.xnu_resets == 1 && g.TmsBusy() == 0);   /* requeued (D20b) */
        m.holdDev = 0; m.tmIgnoreMask = 0;
        CHECK(Resume(g) == 0);
        g.Pump();
        CHECK(a->done && a->status == CAM_REQ_CMP);
        Io *b = g.Read(0x0009, 0, 7);
        CHECK(g.Go(b) == kPosted);
        g.Pump();
        CHECK(b->status == CAM_REQ_CMP && !g.Recovering(0x0009));
    }

    printf("sleep: device removed while asleep -> resume finds it gone, held IO failed, targets dropped\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        CHECK(Suspend(g, 100) == 0);
        Io *r = g.Read(0x0009, 0, 1);
        CHECK(g.Go(r) == kHeld);
        m.dead = true;
        CHECK(Resume(g) == 0);
        CHECK(g.Gone() && r->done && r->status == CAM_DEV_NOT_THERE);
        CHECK(!g.Present(0, 0x0009) && g.AsyncCount(AC_XNU_SCAN) >= 1);
    }

    printf("shutdown: suspend (MUR), then detach without resume frees everything, no register access\n");
    {
        Rig g;
        MockIocModel &m = g.ioc;
        const size_t mark = LogMark();
        const int murs = m.murCount;
        CHECK(Suspend(g, 100) == 0);
        Io *r = g.Read(0x000A, 0, 1);
        CHECK(g.Go(r) == kHeld);
        g.Detach();                              /* mps_free: held IO failed, DMA freed */
        CHECK(r->done && r->status == CAM_DEV_NOT_THERE);
        CHECK(m.murCount == murs + 1 && !HasLog(mark, "accessed while suspended"));
    }
}

/* ---- review fixes (docs/reviews/2026-11-final.md) ---------------------------
 * Each scenario failed on the code before its fix (REVIEW_BEFORE builds them
 * against the old API; MOCK_ONLY=<id> runs one).
 */
#ifdef REVIEW_BEFORE
static int Shutdown(Rig &g, u_int drainMs) { mps_lock(g.sc); int rc = mps_xnu_suspend(g.sc, drainMs); mps_unlock(g.sc); return rc; }
static uint32_t Gen(Rig &, target_id_t) { return 0; }
#else
static int Shutdown(Rig &g, u_int drainMs) { mps_lock(g.sc); int rc = mps_xnu_suspend(g.sc, drainMs, 1); mps_unlock(g.sc); return rc; }
static uint32_t Gen(Rig &g, target_id_t t) { return mps_xnu_target_gen(g.sc, t); }
#endif

static bool Want(const char *id)
{
    const char *only = getenv("MOCK_ONLY");
    if (only && strcmp(only, id) != 0)
        return false;
    printf("review %s\n", id);
    return true;
}

static bool NoneUnverified(struct mps_softc *sc)
{
    for (u_int i = 0; i < sc->xnu.ntids; i++)
        if (sc->xnu.tids[i].unverified)
            return false;
    return true;
}

static void RequestReinit(Rig &g, const char *why)
{
    mps_lock(g.sc);
    mps_xnu_request_reinit(g.sc, why);
    mps_unlock(g.sc);
}

static void ReviewTests()
{
    if (Want("H2-inline")) {
        /* a TM timeout (any reinit request) inside the diag reset of an in-line reinit */
        Rig g;
        MockIocModel &m = g.ioc;
        const int resets = m.diagResetCount;
        bool fired = false;
        gHost.onOtherThreads = [&]() {
            if (!fired && g.InReset()) { fired = true; RequestReinit(g, "task management timeout (test)"); }
        };
        CHECK(g.ForceReinit() == 0);            /* mps_wait_command's in-line reinit */
        HostAdvance(2000);
        gHost.onOtherThreads = nullptr;
        CHECK(fired);
        CHECK(!g.ReinitQueued());               /* lost: new IO would be requeued for good */
        CHECK(m.diagResetCount == resets + 1);  /* doubled: a second diag reset */
        Io *r = g.Read(0x0009, 0, 3);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("H2-task")) {
        /* the running reset is the reinit task itself */
        Rig g;
        MockIocModel &m = g.ioc;
        const int resets = m.diagResetCount;
        bool fired = false;
        gHost.onOtherThreads = [&]() {
            if (!fired && g.InReset()) { fired = true; RequestReinit(g, "task management timeout (test)"); }
        };
        RequestReinit(g, "IOC fault (test)");
        HostAdvance(2000);
        gHost.onOtherThreads = nullptr;
        CHECK(fired && !g.ReinitQueued() && !g.InReset());
        CHECK(m.diagResetCount == resets + 1);
        Io *r = g.Read(0x000A, 0, 4);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("H4")) {
        /* a whole enclosure goes: 7 removals in one event batch, 4 TMs */
        const std::vector<uint16_t> seven = {0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F};
        Rig g(seven, [](Rig &r) { r.sc->max_prireqframes = 4; r.ioc.tmSmidHi = 1 + 4; });
        MockIocModel &m = g.ioc;
        std::vector<target_id_t> tids;
        for (uint16_t h : seven) tids.push_back(g.Tid(h));
        const int removes = m.removeDevices;
        std::vector<int> scans;
        for (target_id_t t : tids) scans.push_back(g.Scans(t));
        m.PostTopologyEvent(seven, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
        HostRunPending();
        g.Pump();
        HostRunPending();
        CHECK(m.removeDevices == removes + 7);
        for (size_t i = 0; i < tids.size(); i++) {
            CHECK(g.Scans(tids[i]) == scans[i] + 1);        /* the OS heard of every removal */
            CHECK(!mps_xnu_target_present(g.sc, tids[i], nullptr));
            CHECK(g.sc->sassc->targets[tids[i]].handle == 0);
        }
        CHECK(g.TmsBusy() == 0);
    }

    if (Want("H5")) {
        /* shutdown: IO fails at once afterwards; a wedged IOC: bounded, bus mastering off, no diag reset */
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdIo = true;
        Io *a = g.Read(0x0009, 0, 1);
        CHECK(g.Go(a) == kPosted);
        m.murIgnored = true;
        const uint64_t t0 = m.nowUs;
        const int dead = gHost.dead, diags = m.diagResetCount;
        Shutdown(g, 2000);
        CHECK(m.nowUs - t0 < 5000000);          /* far below the 30 s halt watchdog */
        CHECK(gHost.dead == dead + 1 && m.diagResetCount == diags);
        CHECK(a->done && a->deadAtDone == dead + 1);
        Io *b = g.Read(0x000A, 0, 2);
        CHECK(g.Go(b) == kDone && b->status == CAM_DEV_NOT_THERE);
        m.holdIo = false;
    }

    if (Want("H5-retry")) {
        /* the R/W retry of IO the shutdown stop failed must not be held */
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdIo = true;
        Io *a = g.Read(0x0009, 0, 1);
        CHECK(g.Go(a) == kPosted);
        Shutdown(g, 100);
        CHECK(a->done);
        Io *retry = g.Read(0x0009, 0, 1);
        CHECK(g.Go(retry) == kDone && retry->status == CAM_DEV_NOT_THERE);
        CHECK(g.HeldCount() == 0);
        m.holdIo = false;
    }

    if (Want("M4")) {
        /* sleep with an IOC that does not stop: bus mastering off before any buffer goes back */
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdIo = true;
        Io *a = g.Read(0x0009, 0, 1);
        CHECK(g.Go(a) == kPosted);
        m.murIgnored = true;
        const int dead = gHost.dead;
        const uint64_t t0 = m.nowUs;
        Suspend(g, 100);
        CHECK(m.nowUs - t0 < 15000000);
        CHECK(gHost.dead == dead + 1);
        CHECK(a->done ? a->deadAtDone == dead + 1 : a->ccb->ccb_h.xnu_held);   /* D20b: requeued, held */
        m.holdIo = false;
        g.Detach();
        CHECK(a->done && a->deadAtDone >= dead + 1);
    }

    if (Want("M2")) {
        /* detach with device adds queued: each add's page read would wait 60 s on a stopped IOC */
        Rig g;
        MockIocModel &m = g.ioc;
        for (uint16_t h : {0x0010, 0x0011, 0x0012}) {
            m.devices.push_back(h);
            m.PostTopologyEvent({h});
        }
        g.Pump();                               /* queued on the event taskqueue, not run */
        const uint64_t t0 = m.nowUs;
        g.Detach();
        CHECK(m.nowUs - t0 < 10000000);
    }

    if (Want("M3")) {
        /* fail every allocation of mps_attach in turn: no double mps_free, nothing leaked */
        int failed = 0;
        for (long n = 1; n < 400; n++) {
            Rig::failAllocN = n;
            if (getenv("M3_TRACE")) printf("  n=%ld\n", n);
            const int before = gHost.allocFailures;
            Rig g({0x0009}, nullptr, false);
            Rig::failAllocN = 0;
            if (gHost.allocFailures == before) { CHECK(g.attachRc == 0); break; }   /* past the last one */
            failed += g.attachRc != 0;
        }
        printf("  %d injected failures made mps_attach fail\n", failed);
        CHECK(failed > 10);
    }

    if (Want("M6")) {
        /* SYNCHRONIZE CACHE in flight at a reinit: re-sent (the OS retries only READ/WRITE; D20b: those too) */
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdIo = true;
        uint8_t sync[10] = {0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        Io *s = g.NewIo(g.Tid(0x0009), 0, CAM_DIR_NONE, sync, 10, {});
        CHECK(g.Go(s) == kPosted);
        Io *r = g.Read(0x0009, 0, 4);
        CHECK(g.Go(r) == kPosted);
        m.holdIo = false;
        RequestReinit(g, "test");
        HostAdvance(2000);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);           /* READ: re-sent too since D20b */
        CHECK(s->done && s->status == CAM_REQ_CMP);           /* re-sent by us after the reinit */
        /* bounded: killed by 4 resets in a row, the 4th failure goes up */
        m.holdIo = true;
        Io *s2 = g.NewIo(g.Tid(0x0009), 0, CAM_DIR_NONE, sync, 10, {});
        CHECK(g.Go(s2) == kPosted);
        for (int i = 0; i < 4 && !s2->done; i++) {
            RequestReinit(g, "test");
            HostAdvance(2000);
            g.Pump();
        }
        CHECK(s2->done && s2->status == CAM_SCSI_BUS_RESET);
        m.holdIo = false;
    }

    if (Want("M8")) {
        /* the tunnel passes MMIO only 500 ms after the power-on: not a surprise removal */
        Rig g;
        MockIocModel &m = g.ioc;
        CHECK(Suspend(g, 100) == 0);
        m.deadUntilUs = m.nowUs + 500000;
        CHECK(Resume(g) == 0);
        HostAdvance(3000);
        CHECK(!g.Gone() && g.Present(g.Tid(0x0009), 0x0009));
        Io *r = g.Read(0x0009, 0, 5);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->status == CAM_REQ_CMP);
    }

    if (Want("M1")) {
        /* system sleep while a reinit sleeps in its diag reset: not disabled, works after wake */
        Rig g;
        bool fired = false;
        int rc = -1;
        gHost.onOtherThreads = [&]() {
            if (!fired && g.InReset() && !g.sc->xnu.suspending) { fired = true; rc = Suspend(g, 100); }
        };
        RequestReinit(g, "IOC fault (test)");
        HostAdvance(3000);
        gHost.onOtherThreads = nullptr;
        CHECK(fired && rc == 0);
        CHECK(!g.Gone() && !g.sc->xnu.disabled);
        CHECK(Resume(g) == 0);
        HostAdvance(3000);
        CHECK(!g.Gone() && !g.InReset() && !g.ReinitQueued());
        CHECK(g.Present(g.Tid(0x000A), 0x000A));
        Io *r = g.Read(0x000A, 0, 6);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->status == CAM_REQ_CMP);
    }

    if (Want("C2-swap")) {
        /* SATA disks: another disk in the same bay across a reinit (sleep) is not the same target */
        Rig g({0x0009, 0x000B}, [](Rig &r) {
            r.ioc.sataSerial[0x0009] = "ZR50AAAA";
            r.ioc.sataSerial[0x000B] = "ZR50BBBB";
        });
        MockIocModel &m = g.ioc;
        CHECK(m.identifies >= 2);
        const target_id_t t9 = g.Tid(0x0009), tb = g.Tid(0x000B);
        const uint32_t g9 = Gen(g, t9), gb = Gen(g, tb);
        RequestReinit(g, "same disks");
        HostAdvance(2000);
        CHECK(g.Tid(0x0009) == t9 && g.Tid(0x000B) == tb);
        CHECK(Gen(g, t9) == g9 && Gen(g, tb) == gb);         /* same disks: the nubs stay */
        m.sataSerial[0x000B] = "ZR50CCCC";                   /* disk B pulled, disk C in its bay */
        RequestReinit(g, "disk swapped");
        HostAdvance(2000);
        const target_id_t tc = g.Tid(0x000B);
        CHECK(tc != tb || Gen(g, tc) != gb);                  /* never B's nub for C */
        CHECK(!(tc == tb && Gen(g, tc) == gb));
        CHECK(g.Tid(0x0009) == t9 && Gen(g, t9) == g9);
        m.sataSerial[0x000B] = "ZR50BBBB";                   /* B back in its bay: B's id again */
        RequestReinit(g, "disk back");
        HostAdvance(2000);
        CHECK(g.Tid(0x000B) == tb);
    }

    if (Want("C2-permute")) {
        /* handles exchanged across a reset: each disk keeps its target */
        Rig g({0x0009, 0x000B}, [](Rig &r) {
            r.ioc.sataSerial[0x0009] = "ZR50AAAA";
            r.ioc.sataSerial[0x000B] = "ZR50BBBB";
        });
        MockIocModel &m = g.ioc;
        const target_id_t ta = g.Tid(0x0009), tb = g.Tid(0x000B);
        std::swap(m.sataSerial[0x0009], m.sataSerial[0x000B]);
        RequestReinit(g, "handles permuted");
        HostAdvance(2000);
        CHECK(g.Tid(0x000B) == ta && g.Tid(0x0009) == tb);
    }

    if (Want("C2-sas")) {
        /* SAS disks: the page 0 SAS address identifies the disk */
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        const target_id_t t = g.Tid(0x0009);
        const uint32_t gen = Gen(g, t);
        m.sasAddr[0x0009] = 0x5000C500DEADBEEFull;
        RequestReinit(g, "disk swapped");
        HostAdvance(2000);
        const target_id_t t2 = g.Tid(0x0009);
        CHECK(!(t2 == t && Gen(g, t2) == gen));
    }
}

/* ---- FreeBSD deviation fixes (docs/reviews/2026-11-freebsd-deviations.md) ----
 * Each scenario failed on the code before its fix; MOCK_ONLY=D<n> runs one.
 */
static void DeviationTests()
{
    if (Want("D1")) {
        /* a SATA SSD (IDENTIFY word 217 = 1) next to a SAS disk; any TM freed */
        Rig g({0x0009, 0x000B}, [](Rig &r) {
            r.ioc.sataSerial[0x000B] = "S6SSD001";
            r.ioc.sataSsd.insert(0x000B);
        });
        MockIocModel &m = g.ioc;
        const target_id_t ts = g.Tid(0x000B);
        CHECK(g.Targ(0x000B)->flags & MPS_TARGET_IS_SATA_SSD);
        const int removes = m.removeDevices;
        const size_t tms = m.tms.size();
        Io *t = g.ResetDevSync(g.Tid(0x0009));          /* an OS TM on the other disk */
        CHECK(t->status == CAM_REQ_CMP);
        HostRunPending();
        g.Pump();
        CHECK(m.removeDevices == removes);               /* the SSD is not removed */
        for (size_t i = tms; i < m.tms.size(); i++)
            CHECK(m.tms[i].dev != 0x000B);               /* nor reset */
        CHECK(g.Present(ts, 0x000B));
        CHECK(g.Targ(0x000B)->flags & MPS_TARGET_IS_SATA_SSD);
        Io *r = g.Read(0x000B, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D7")) {
        /* 70 end devices behind an expander, MaxTargets 128: every one gets a target id (FreeBSD: MaxTargets) */
        std::vector<uint16_t> devs;
        for (uint16_t i = 0; i < 70; i++)
            devs.push_back((uint16_t)(0x0100 + i));
        Rig g(devs, [](Rig &r) { r.ioc.maxTargets = 128; });
        std::set<target_id_t> ids;
        for (uint16_t h : devs) {
            CHECK(g.Targ(h) != nullptr);
            const target_id_t t = g.Tid(h);
            CHECK(t < 128 && g.Present(t, h));
            ids.insert(t);
        }
        CHECK(ids.size() == 70 && *ids.rbegin() >= 64);
        Io *r = g.Read(0x0100 + 69);                    /* IO to the 70th */
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D29")) {
        /* FreeBSD never sends EVENT_ACK (AckRequired is only printed): discovery and a hot-add
         * complete with no ack, nothing logged about acks */
        const size_t mark = LogMark();
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        CHECK(m.ackCount == 0 && g.Present(g.Tid(0x0009), 0x0009));
        m.devices.push_back(0x000B);
        m.PostTopologyEvent({0x000B});                  /* a hot-add, AckRequired set */
        HostRunPending();
        g.Pump();
        CHECK(g.Present(g.Tid(0x000B), 0x000B));
        CHECK(m.ackCount == 0 && !HasLog(mark, "asks for an ack"));
    }

    if (Want("D55")) {
        /* our KASSERT and panic() log and return: counted, so a hardware run can be checked for them */
        Rig g({0x0009});
        const uint32_t before = g.Stats().kassert_hits;
        gHost.panicsAllowed = 2;
        KASSERT(g.sc == nullptr, ("D55 test %d", 1));
        mps_compat_panic("D55 test %d", 2);
        gHost.panicsAllowed = 0;
        CHECK(gHost.panics >= 2 && g.Stats().kassert_hits == before + 2);
    }

    if (Want("L12")) {
        /* free() of a pointer malloc did not return: KASSERT, and the block is not touched (FreeBSD
         * never frees a bad pointer); no IOFree with a garbage size, no DMA unload of a garbage map */
        Rig g({0x0009});
        const long n0 = mps_compat_malloc_count();
        const uint32_t before = g.Stats().kassert_hits;
        char *p = (char *)mps_compat_malloc(64, nullptr, M_WAITOK | M_ZERO);
        CHECK(p != nullptr && mps_compat_malloc_count() == n0 + 1);
        gHost.panicsAllowed = 1;
        mps_compat_free(p + 32, nullptr);               /* "header" = zeroed data: bad magic */
        gHost.panicsAllowed = 0;
        CHECK(g.Stats().kassert_hits == before + 1 && mps_compat_malloc_count() == n0 + 1);
        mps_compat_free(p, nullptr);
        CHECK(mps_compat_malloc_count() == n0);
    }

    if (Want("L13")) {
        /* IOCFacts changed across a diag reset -> mpssas_realloc_targets; its allocation fails
         * (IOMalloc can return NULL): the old array stays, zeroed, and rediscovery fills it */
        Rig g({0x0009, 0x000A});
        MockIocModel &m = g.ioc;
        struct mpssas_target *old = g.sc->sassc->targets;
        const int maxt = g.sc->sassc->maxtargets;
        m.maxTargets = 40;                              /* the facts differ: reallocating */
        const int fails = gHost.allocFailures;
        gHost.failMallocSize = 32 + sizeof(struct mpssas_target) * maxt;   /* compat header + the array */
        CHECK(g.ForceReinit() == 0);
        CHECK(gHost.allocFailures == fails + 1 && gHost.failMallocSize == 0);
        CHECK(g.sc->sassc->targets == old && (int)g.sc->sassc->maxtargets == maxt);
        CHECK(g.sc->mps_flags & MPS_FLAGS_REALLOCATED);
        HostAdvance(3000);
        g.Pump();
        CHECK(g.Present(g.Tid(0x0009), 0x0009) && g.Present(g.Tid(0x000A), 0x000A));
        Io *r = g.Read(0x000A, 0, 5);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D30")) {
        /* an interrupt that finds an empty queue reads no register; a fault is found by mps_periodic (1 s) */
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        const uint64_t reads = m.doorbellReads;
        for (int i = 0; i < 100; i++)
            g.Interrupt();                              /* MSI races, polled callers */
        CHECK(m.doorbellReads == reads);
        m.Fault(0x5854);
        g.Interrupt();
        CHECK(!g.Faulted() && m.doorbellReads == reads);
        m.nowUs += 1100000;
        g.Callouts();                                   /* mps_periodic: FreeBSD's fault check */
        CHECK(g.Faulted() && g.Stats().faults == 1);
        CHECK(g.RunReinit() == 0 && !g.Faulted());
    }

    if (Want("D2")) {
        /* hot-add of a SATA disk whose IDENTIFY never comes back (a bad disk) */
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        m.sataSerial[0x000B] = "ZR50BAD0";
        m.identifyStuck.insert(0x000B);
        m.devices.push_back(0x000B);
        const size_t tms = m.tms.size();
        m.PostTopologyEvent({0x000B});
        HostRunPending();
        g.Pump();
        HostRunPending();
        CHECK(m.stuckIds.empty());                       /* the target reset made the IOC answer */
        bool reset = false;
        for (size_t i = tms; i < m.tms.size(); i++)
            reset |= m.tms[i].dev == 0x000B && m.tms[i].type == kTgtT;
        CHECK(reset);
        CHECK(mps_compat_bufmap_count() == 0);                     /* the late completion gave the buffer back */
        CHECK(g.TmsBusy() == 0);
        /* page 0 reads and a second hot-add work again */
        Mpi2ConfigReply_t mpi_reply;
        Mpi2SasDevicePage0_t pg;
        mps_lock(g.sc);
        int rc = mps_config_get_sas_device_pg0(g.sc, &mpi_reply, &pg,
            MPI2_SAS_DEVICE_PGAD_FORM_HANDLE, 0x0009);
        mps_unlock(g.sc);
        CHECK(rc == 0);
        m.devices.push_back(0x000C);
        m.PostTopologyEvent({0x000C});
        HostRunPending();
        CHECK(g.Targ(0x000C) != nullptr && g.Present(g.Tid(0x000C), 0x000C));
        CHECK(g.Targ(0x000B) != nullptr && g.Present(g.Tid(0x000B), 0x000B));
        Io *r = g.Read(0x000B, 0, 3);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D6")) {
        /* config pages and IDENTIFY in malloc()'d buffers, mapped at load (no single lent buffer) */
        Rig g({0x0009});
        MockIocModel &m = g.ioc;
        m.sataSerial[0x000B] = "ZR50BAD0";
        m.identifyStuck.insert(0x000B);
        m.devices.push_back(0x000B);
        int reads = 0, ok = 0;
        gHost.onOtherThreads = [&]() {
            if (reads || m.stuckIds.empty()) return;    /* the IDENTIFY is in the IOC, its buffer mapped */
            reads++;
            CHECK(mps_compat_bufmap_count() == 1);
            Mpi2ConfigReply_t rep;
            Mpi2SasDevicePage0_t pg;
            Mpi2IOUnitPage7_t p7;
            mps_lock(g.sc);
            ok += mps_config_get_sas_device_pg0(g.sc, &rep, &pg, MPI2_SAS_DEVICE_PGAD_FORM_HANDLE, 0x0009) == 0;
            ok += mps_config_get_iounit_pg7(g.sc, &rep, &p7) == 0;
            mps_unlock(g.sc);
        };
        m.PostTopologyEvent({0x000B});
        HostRunPending();
        g.Pump();
        HostRunPending();
        gHost.onOtherThreads = nullptr;
        CHECK(reads == 1 && ok == 2);                   /* both read while the IDENTIFY was stuck */
        CHECK(mps_compat_bufmap_count() == 0 && m.liveMaps == 0);
        /* FreeBSD's sync page reads never unload: 1000 reads leave no mapping behind */
        const int loads = gHost.bufLoads;
        for (int i = 0; i < 1000; i++) {
            Mpi2ConfigReply_t rep;
            Mpi2SasDevicePage0_t pg;
            mps_lock(g.sc);
            CHECK(mps_config_get_sas_device_pg0(g.sc, &rep, &pg, MPI2_SAS_DEVICE_PGAD_FORM_HANDLE, 0x0009) == 0);
            mps_unlock(g.sc);
        }
        CHECK(gHost.bufLoads == loads + 1000 && mps_compat_bufmap_count() == 0 && m.liveMaps == 0);
        /* free() of a loaded block drops its mapping; so does the next load of the map */
        bus_dmamap_t map;
        CHECK(bus_dmamap_create(g.sc->buffer_dmat, 0, &map) == 0);
        auto cb = [](void *, bus_dma_segment_t *, int nsegs, int error) { CHECK(error == 0 && nsegs == 1); };
        void *b1 = mps_compat_malloc(512, NULL, M_NOWAIT | M_ZERO);
        void *b2 = mps_compat_malloc(1024, NULL, M_NOWAIT | M_ZERO);
        CHECK(bus_dmamap_load(g.sc->buffer_dmat, map, b1, 512, cb, nullptr, 0) == 0);
        CHECK(mps_compat_bufmap_count() == 1 && m.liveMaps == 1);
        CHECK(bus_dmamap_load(g.sc->buffer_dmat, map, b2, 1024, cb, nullptr, 0) == 0);
        CHECK(mps_compat_bufmap_count() == 1 && m.liveMaps == 1);   /* b1's mapping released */
        mps_compat_free(b1, NULL);
        CHECK(mps_compat_bufmap_count() == 1);
        mps_compat_free(b2, NULL);
        CHECK(mps_compat_bufmap_count() == 0 && m.liveMaps == 0);
        bus_dmamap_destroy(g.sc->buffer_dmat, map);
        /* the target with the bad IDENTIFY was added after its target reset */
        CHECK(g.Targ(0x000B) != nullptr);
    }

    if (Want("D11")) {
        /* one all-ones register read (a link retrain, not an unplug): confirmed before the controller is dropped */
        Rig g;
        MockIocModel &m = g.ioc;
        const int resets = m.diagResetCount, dead = gHost.dead;
        m.deadUntilUs = m.nowUs + 1000;                  /* MMIO reads all-ones for 1 ms, config space fine */
        mps_lock(g.sc);
        CHECK(mps_regread(g.sc, MPI2_DOORBELL_OFFSET) == 0xffffffff);
        mps_unlock(g.sc);
        HostAdvance(500);
        CHECK(!g.Gone() && !g.sc->xnu.disabled && gHost.dead == dead);
        CHECK(g.Stats().transient_allones == 1 && m.diagResetCount == resets);
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
        /* unplugged: MMIO and config space read all-ones on every try */
        m.dead = true;
        HostAdvance(1100);
        HostAdvance(200);
        CHECK(g.Gone() && g.sc->xnu.disabled && gHost.dead == dead + 1);
        CHECK(g.Stats().transient_allones == 1);
    }

    if (Want("D19")) {
        /* PCI state restored after the reinit's diag reset (one attempt since D55b) */
        Rig g;
        MockIocModel &m = g.ioc;
        const int restores = gHost.pciRestores, resets = m.diagResetCount;
        RequestReinit(g, "test");
        HostAdvance(5000);
        CHECK(!g.Gone() && m.diagResetCount == resets + 1);
        CHECK(gHost.pciRestores == restores + 1);
    }

    if (Want("D12")) {
        /* wake: the IOC kept power and is READY (our suspend's MUR): FreeBSD's attach sequence, no diag reset */
        Rig g({0x0009, 0x000A});
        MockIocModel &m = g.ioc;
        const target_id_t t9 = g.Tid(0x0009);
        const uint32_t g9 = Gen(g, t9);
        int resets = m.diagResetCount, inits = m.iocInits, pe = m.portEnables;
        CHECK(Suspend(g, 1000) == 0 && m.state == MPI2_IOC_STATE_READY);
        CHECK(Resume(g) == 0);
        HostAdvance(3000);
        CHECK(m.diagResetCount == resets && m.iocInits == inits + 1 && m.portEnables == pe + 1);
        CHECK(g.Stats().resume_ready == 1 && g.Stats().resume_diag_reset == 0);
        CHECK(g.Tid(0x0009) == t9 && Gen(g, t9) == g9);
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
        /* that sequence fails (IOCInit faults): the diag reset as before */
        resets = m.diagResetCount;
        CHECK(Suspend(g, 1000) == 0);
        m.faultOnIocInitOnce = true;
        CHECK(Resume(g) == 0);
        HostAdvance(5000);
        CHECK(m.diagResetCount == resets + 1 && !g.Gone());
        CHECK(g.Stats().resume_ready == 1 && g.Stats().resume_diag_reset == 1);
        Io *r2 = g.Read(0x000A, 0, 3);
        CHECK(g.Go(r2) == kPosted);
        g.Pump();
        CHECK(r2->done && r2->status == CAM_REQ_CMP);
    }

    if (Want("D37")) {
        /* cam_compat.h: CAM's own values (cam_ccb.h, cam.h), as its header claims */
        CHECK(XPT_SCSI_IO == 0x901 && XPT_RESET_DEV == 0x912 && XPT_ABORT == 0x10);
        CHECK(XPT_SCAN_BUS == 0x708 && XPT_SCAN_LUN == 0x714 && XPT_SCAN_TGT == 0x71e);
        CHECK(XPT_RESET_BUS == 0x411 && XPT_TERM_IO == 0x13 && XPT_SMP_IO == 0x91b);
        CHECK(AC_ADVINFO_CHANGED == 0x2000 && CAM_PRIORITY_NORMAL == 0x480);
        /* device-queued ccbs are still held behind a frozen queue */
        Rig g;
        mps_lock(g.sc);
        xpt_freeze_simq(g.sc->sassc->sim, 1);
        mps_unlock(g.sc);
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kHeld);
        mps_lock(g.sc);
        xpt_release_simq(g.sc->sassc->sim, 1);
        mps_unlock(g.sc);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("L4")) {
        /* a devq freeze whose entry cannot be allocated holds everything (embedded fallback entry), and
         * the matching device release finds and undoes it, as CAM's plain qfrozen_cnt; a SIM release
         * does not consume it */
        Rig g;
        struct cam_sim *sim = g.sc->sassc->sim;
        const target_id_t t9 = g.Tid(0x0009), tA = g.Tid(0x000A);
        struct cam_path *p9;
        CHECK(xpt_create_path(&p9, NULL, cam_sim_path(sim), t9, 0) == CAM_REQ_CMP);
        mps_lock(g.sc);
        gHost.failAllocAt = gHost.allocs + 1;               /* the freeze entry's malloc */
        xpt_freeze_devq(p9, 1);
        gHost.failAllocAt = 0;
        mps_unlock(g.sc);
        Io *r = g.Read(0x000A, 0, 2);                       /* another target: held too (wider hold) */
        CHECK(tA != t9 && g.Go(r) == kHeld);
        mps_lock(g.sc);
        xpt_freeze_simq(sim, 1);
        xpt_release_simq(sim, 1);                           /* not the devq's freeze */
        mps_unlock(g.sc);
        g.Pump();
        CHECK(!r->done && sim->freeze == 0 && !TAILQ_EMPTY(&sim->frozen));
        mps_lock(g.sc);
        xpt_release_devq(p9, 1, TRUE);
        mps_unlock(g.sc);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
        CHECK(TAILQ_EMPTY(&sim->frozen) && sim->freeze == 0 && g.HeldCount() == 0);
        xpt_free_path(p9);
    }

    if (Want("L11")) {
        /* a callout handler runs with its mutex held and may not drop it (FreeBSD: THREAD_NO_SLEEPING
         * around c_func, softclock unlocks after it): an unlock/sleep or a drain of itself from the
         * handler is a KASSERT, which is what makes callout_drain == callout_stop safe */
        Rig g({0x0009});
        static struct callout c;
        static int ran;
        ran = 0;
        callout_init_mtx(&c, &g.sc->mps_mtx, 0);
        const uint32_t before = g.Stats().kassert_hits;
        mps_lock(g.sc);
        callout_reset(&c, 1, [](void *arg) {
            struct mps_softc *sc = (struct mps_softc *)arg;
            ran++;
            mtx_unlock(&sc->mps_mtx);                   /* drops the callout's mutex */
            mtx_lock(&sc->mps_mtx);
            callout_drain(&c);                          /* drains itself */
        }, g.sc);
        mps_unlock(g.sc);
        gHost.panicsAllowed = 2;
        g.ioc.nowUs += 2000;
        g.Callouts();
        gHost.panicsAllowed = 0;
        CHECK(ran == 1 && g.Stats().kassert_hits == before + 2);
        /* the driver's own handlers never do: an unexpected KASSERT aborts every other test */
    }

    if (Want("L3")) {
        /* a held ccb can always be cancelled, as CAM's XPT_ABORT of a queued ccb: also while a run of
         * the held queue has it in its batch (the tail and a middle element), and a flush sees them */
        Rig g;
        struct cam_sim *sim = g.sc->sassc->sim;
        static sim_action_func *orig;
        static union ccb *first, *cancel[2];
        static int cancelled;
        mps_lock(g.sc);
        xpt_freeze_simq(sim, 1);
        mps_unlock(g.sc);
        Io *io[4];
        for (int i = 0; i < 4; i++) {
            io[i] = g.Read(0x0009, 0, 10 + i);
            CHECK(g.Go(io[i]) == kHeld);
        }
        first = io[0]->ccb; cancel[0] = io[3]->ccb; cancel[1] = io[1]->ccb;   /* the tail, a middle one */
        cancelled = 0;
        orig = sim->sim_action;
        sim->sim_action = [](struct cam_sim *s, union ccb *ccb) {
            if (ccb == first)
                for (union ccb *c : cancel)
                    cancelled += cam_xnu_cancel_held(s, c, CAM_REQ_ABORTED);
            orig(s, ccb);
        };
        mps_lock(g.sc);
        xpt_release_simq(sim, 1);
        mps_unlock(g.sc);
        sim->sim_action = orig;
        g.Pump();
        CHECK(cancelled == 2);
        CHECK(io[3]->done && io[3]->status == CAM_REQ_ABORTED && io[1]->done && io[1]->status == CAM_REQ_ABORTED);
        CHECK(io[0]->done && io[0]->status == CAM_REQ_CMP && io[2]->done && io[2]->status == CAM_REQ_CMP);
        CHECK(g.HeldCount() == 0 && TAILQ_EMPTY(&sim->held));
        /* a flush from inside a run fails the rest of the batch, once */
        mps_lock(g.sc);
        xpt_freeze_simq(sim, 1);
        mps_unlock(g.sc);
        Io *f[3];
        for (int i = 0; i < 3; i++) {
            f[i] = g.Read(0x0009, 0, 20 + i);
            CHECK(g.Go(f[i]) == kHeld);
        }
        first = f[0]->ccb;
        orig = sim->sim_action;
        sim->sim_action = [](struct cam_sim *s, union ccb *ccb) {
            if (ccb == first)
                cam_xnu_flush_held(s, CAM_DEV_NOT_THERE);
            orig(s, ccb);
        };
        mps_lock(g.sc);
        xpt_release_simq(sim, 1);
        mps_unlock(g.sc);
        sim->sim_action = orig;
        g.Pump();
        CHECK(f[0]->done == 1 && f[0]->status == CAM_REQ_CMP);
        CHECK(f[1]->done == 1 && f[1]->status == CAM_DEV_NOT_THERE && f[2]->done == 1 && f[2]->status == CAM_DEV_NOT_THERE);
        CHECK(g.HeldCount() == 0 && g.sc->io_cmds_active == 0);
    }

    if (Want("D27")) {
        /* the kernel printf's %b in compat: FreeBSD's call sites stay verbatim */
        char b[200];
        mps_compat_snprintf(b, sizeof(b), "%b", 0x5, "\20\1A\3C");
        CHECK(strcmp(b, "5<A,C>") == 0);
        mps_compat_snprintf(b, sizeof(b), "%b", 0, "\20\1A");
        CHECK(strcmp(b, "0") == 0);
        mps_compat_snprintf(b, sizeof(b), "%b", 0x80000000u, "\20\40Top");
        CHECK(strcmp(b, "80000000<Top>") == 0);
        mps_compat_snprintf(b, sizeof(b), "%b", 9, "\10\1A\4D");
        CHECK(strcmp(b, "11<A,D>") == 0);
        /* other conversions around it keep their arguments */
        mps_compat_snprintf(b, sizeof(b), "%s %04x %b %ju %-3d|%.*s %zu %lld%%", "a", 0x2b, 3,
                            "\20\1X\2Y", (uintmax_t)1 << 40, 7, 2, "xyz", (size_t)9, -5ll);
        CHECK(strcmp(b, "a 002b 3<X,Y> 1099511627776 7  |xy 9 -5%") == 0);
        CHECK(mps_compat_snprintf(b, 6, "%b!", 0x3, "\20\1Long\2Names") == 14 && strcmp(b, "3<Lon") == 0);
        /* a FreeBSD site: mps_print_iocfacts' IOCCapabilities, through mps_compat_log */
        Rig g({0x0009}, [](Rig &r) { r.ioc.iocCaps = 0x1C; });
        const size_t mark = LogMark();
        mps_lock(g.sc);
        mps_print_iocfacts(g.sc, g.sc->facts);
        mps_unlock(g.sc);
        CHECK(HasLog(mark, "IOCCapabilities: 1c<ScsiTaskFull,DiagTrace,SnapBuf>"));
    }

    if (Want("D16")) {
        /* DELAY keeps mps_mtx, as FreeBSD's spin: the doorbell handshake of a reinit is atomic */
        Rig g;
        struct mps_softc *sc = g.sc;
        int lockedLong = 0, droppedLong = 0;
        gHost.onDelay = [&](uint64_t us) {
            if (us < 1000) return;
            if (mtx_owned(&sc->mps_mtx)) lockedLong++;
            else if (gHost.locksHeld == 0) droppedLong++;
        };
        mps_lock(sc);
        DELAY(2000);
        CHECK(mtx_owned(&sc->mps_mtx));
        mps_unlock(sc);
        CHECK(lockedLong == 1);
        CHECK(g.ForceReinit() == 0);                    /* IOCFacts/IOCInit handshakes: DELAY(2000) per word */
        gHost.onDelay = nullptr;
        CHECK(lockedLong > 1);
        HostAdvance(5000);                              /* the rediscovery */
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D35")) {
        /* a reinit with 2000 IOs in the IOC and 500 held: the held queue's work stays bounded */
        Rig g({0x0009, 0x000A}, [](Rig &r) { r.ioc.requestCredit = 2100; r.ioc.replyPostDepth = 4096; });
        struct cam_sim *sim = g.sc->sassc->sim;
        std::vector<Io *> posted = HoldIo(g, {0x0009, 0x000A}, 1000);
        RequestReinit(g, "test");
        std::vector<Io *> held;
        for (int i = 0; i < 500; i++) {
            Io *r = g.Read(i & 1 ? 0x0009 : 0x000A, 0, (uint8_t)i);
            CHECK(g.Go(r) == kHeld);                /* a reinit is pending: requeued, SIM frozen */
            held.push_back(r);
        }
        CHECK(g.HeldCount() == 500);
        const uint64_t checks = sim->held_checks;
        g.ioc.holdIo = false;
        HostAdvance(5000);                          /* the reinit: 2000 completions, requeued */
        g.Pump();
        HostAdvance(5000);
        g.Pump();
        int ok = 0;
        for (Io *r : posted) ok += r->done && r->status == CAM_REQ_CMP;
        for (Io *r : held) ok += r->done && r->status == CAM_REQ_CMP;
        CHECK(ok == 2500 && g.HeldCount() == 0);
        CHECK(sim->held_checks - checks <= 4 * 2500);   /* was ~ 2000 x 2500 (each completion rescanned) */
    }

    if (Want("D67")) {
        /* a requeue no freeze backs (nothing would ever re-send it): the assert catches it */
        Rig g;
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
        r->done = 0;
        r->ccb->ccb_h.status = CAM_REQUEUE_REQ;
        gHost.panicsAllowed = 1;
        const int panics = gHost.panics;
        mps_lock(g.sc);
        xpt_done(r->ccb);
        CHECK(gHost.panics == panics + 1);
        gHost.panicsAllowed = 0;
        cam_xnu_flush_held(g.sc->sassc->sim, CAM_DEV_NOT_THERE);
        mps_unlock(g.sc);
    }

    if (Want("D17")) {
        /* a TM's (tid, lun) freeze holds the whole target (compat), mpssas_prepare_for_tm's path is FreeBSD's */
        Rig g;
        struct mps_softc *sc = g.sc;
        mps_lock(sc);
        struct mps_command *tm = mps_alloc_high_priority_command(sc);
        CHECK(tm != nullptr);
        MPI2_SCSI_TASK_MANAGE_REQUEST *req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
        req->TaskType = MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK;
        mpssas_prepare_for_tm(sc, tm, g.Targ(0x0009), 1);
        CHECK(tm->cm_ccb != nullptr && tm->cm_ccb->ccb_h.path->lun_id == 1);
        mps_unlock(sc);
        Io *r0 = g.Read(0x0009, 0, 2);                 /* another LUN of that target: held too */
        CHECK(g.Go(r0) == kHeld);
        Io *ro = g.Read(0x000A, 0, 3);                 /* another target: not */
        CHECK(g.Go(ro) == kPosted);
        mps_lock(sc);
        mpssas_free_tm(sc, tm);
        mps_unlock(sc);
        g.Pump();
        CHECK(r0->done && r0->status == CAM_REQ_CMP && ro->done && ro->status == CAM_REQ_CMP);
    }

    if (Want("D40")) {
        /* config_intrhook: a driver instance runs its own hooks only */
        static int ranA, ranB;
        ranA = ranB = 0;
        struct intr_config_hook ha{}, hb{};
        int a = 0, b = 0;
        ha.ich_func = [](void *) { ranA++; };
        ha.ich_arg = &a;
        hb.ich_func = [](void *) { ranB++; };
        hb.ich_arg = &b;
        CHECK(config_intrhook_establish(&hb) == 0 && config_intrhook_establish(&ha) == 0);
        mps_compat_intrhook_run(&a);
        CHECK(ranA == 1 && ranB == 0);                 /* b's HBA has no interrupt yet */
        mps_compat_intrhook_run(&a);
        CHECK(ranA == 1 && ranB == 0);
        mps_compat_intrhook_run(&b);
        CHECK(ranA == 1 && ranB == 1);
    }

    if (Want("D42")) {
        /* bus_dmamap_load errors as busdma: to the callback, returned only if ENOMEM */
        Rig g;
        struct mps_softc *sc = g.sc;
        static int calls, cbError, cbSegs;
        calls = 0; cbError = -1; cbSegs = -1;
        auto cb = [](void *, bus_dma_segment_t *, int nsegs, int error) {
            calls++; cbError = error; cbSegs = nsegs;
        };
        bus_dmamap_t map;
        CHECK(bus_dmamap_create(sc->buffer_dmat, 0, &map) == 0);
        static char buf[64];
        const int rc = bus_dmamap_load(sc->buffer_dmat, map, buf,
                                       sc->buffer_dmat->dt_maxsize + 1, cb, nullptr, 0);
        CHECK(rc == 0 && calls == 1 && cbError == EINVAL && cbSegs == 0);
        bus_dmamap_destroy(sc->buffer_dmat, map);
    }

    if (Want("D41")) {
        /* busdma boundary: inherited from the parent tag, enforced (split) in load_ccb */
        Rig g;
        struct mps_softc *sc = g.sc;
        CHECK(sc->buffer_dmat->dt_boundary == BUS_SPACE_MAXSIZE_32BIT + 1ull);
        bus_dma_tag_t child, child2;
        CHECK(bus_dma_tag_create(sc->buffer_dmat, 1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
                                 1 << 20, 4, 1 << 20, 0, NULL, NULL, &child) == 0);
        CHECK(child->dt_boundary == BUS_SPACE_MAXSIZE_32BIT + 1ull);      /* inherited */
        CHECK(bus_dma_tag_create(sc->buffer_dmat, 1, 0x10000, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL,
                                 NULL, 1 << 20, 2, 1 << 20, 0, NULL, NULL, &child2) == 0);
        CHECK(child2->dt_boundary == 0x10000);                             /* the smaller one */
        static bus_dma_segment_t got[8];
        static int gotN;
        gotN = -1;
        auto cb = [](void *, bus_dma_segment_t *s, int n, int error) {
            gotN = error ? -error : n;
            for (int i = 0; i < n && i < 8; i++) got[i] = s[i];
        };
        bus_dma_segment_t sg[1] = {{0xFFFFF000ull, 0x2000}};    /* crosses 4 GiB */
        union ccb *ccb = (union ccb *)calloc(1, sizeof(union ccb));
        ccb->ccb_h.flags = CAM_DATA_SG_PADDR | CAM_DIR_IN;
        ccb->csio.data_ptr = (uint8_t *)sg;
        ccb->csio.sglist_cnt = 1;
        ccb->csio.dxfer_len = 0x2000;
        CHECK(bus_dmamap_load_ccb(child, NULL, ccb, cb, nullptr, 0) == 0);
        CHECK(gotN == 2 && got[0].ds_addr == 0xFFFFF000ull && got[0].ds_len == 0x1000 &&
              got[1].ds_addr == 0x100000000ull && got[1].ds_len == 0x1000);
        sg[0].ds_addr = 0x1FFFF000ull;                          /* 64 KiB boundary of child2 */
        gotN = -1;
        CHECK(bus_dmamap_load_ccb(child2, NULL, ccb, cb, nullptr, 0) == 0 && gotN == 2);
        sg[0].ds_len = 0x12000;                                 /* three pieces: more than 2 */
        ccb->csio.dxfer_len = 0x12000;
        gotN = -1;
        CHECK(bus_dmamap_load_ccb(child2, NULL, ccb, cb, nullptr, 0) == EFBIG && gotN == -1);
        free(ccb);
        bus_dma_tag_destroy(child);
        bus_dma_tag_destroy(child2);
    }

    if (Want("D39")) {
        /* a taskqueue with its own thread (xnu): drain sleeps until woken, free waits for the thread */
        Rig g;                                          /* a clock and a log for the hooks */
        struct taskqueue *tq = taskqueue_create("t39", M_WAITOK, taskqueue_thread_enqueue, nullptr);
        CHECK(tq != nullptr && taskqueue_start_threads(&tq, 1, PRIBIO, "t39") == 0);
        tq->tq_threads = 1;                             /* as mps_os_taskqueue_start on xnu */
        static int runs;
        runs = 0;
        struct task t1, t2;
        TASK_INIT(&t1, 0, [](void *, int) { runs++; }, nullptr);
        TASK_INIT(&t2, 0, [](void *, int) { runs += 10; }, nullptr);
        CHECK(taskqueue_enqueue(tq, &t1) == 0);
        const int sleeps = gHost.sleeps;
        taskqueue_drain(tq, &t1);                       /* sleeps; the queue's run wakes it */
        CHECK(runs == 1 && gHost.sleeps == sleeps + 1);
        /* the thread runs what is queued at taskqueue_free, then leaves */
        CHECK(taskqueue_enqueue(tq, &t2) == 0);
        gHost.kicked.clear();                           /* only the thread may run it */
        tq->tq_flags |= TQ_FLAGS_STOPPED;
        mps_compat_taskqueue_thread(tq);
        CHECK(runs == 11 && tq->tq_exited == 1);
        taskqueue_free(tq);                             /* the thread has left: no wait */
    }

    if (Want("D33")) {
        /* FreeBSD's loader tunables, as mps.<name> / mps.<unit>.<name> (boot-args, personality) */
        const int unit = Rig::units;
        gHost.tunables["mps.spinup_wait_time"] = "7";
        gHost.tunables["mps.max_evtframes"] = "0x20";
        gHost.tunables["mps." + std::to_string(unit) + ".max_chains"] = "300";
        gHost.tunables["mps." + std::to_string(unit + 1) + ".spinup_wait_time"] = "9";   /* another HBA's */
        gHost.tunables["mps.use_phy_num"] = "x";                                       /* not a number */
        Rig g;
        gHost.tunables = kInfoPlistTunables;
        CHECK(g.sc->spinup_wait_time == 7 && g.sc->max_evtframes == 32);
        CHECK(g.sc->max_chains == 300 && g.sc->num_chains <= 300);
        CHECK(g.sc->use_phynum == 1);
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D32")) {
        /* maxio: FreeBSD's MPS_MAXIO_PAGES (-1) in the core, 256 pages from the personality */
        {
            Rig g;
            CHECK(g.sc->max_io_pages == kMaxIoPages && g.sc->maxio == kMaxIoPages * 4096);
            CHECK(g.MaxSegs() == (uint32_t)kMaxIoPages + 1);
        }
        gHost.tunables.clear();                     /* no key: FreeBSD's default, the IOC's limit */
        Rig g;
        gHost.tunables = kInfoPlistTunables;
        CHECK(g.sc->max_io_pages == -1);
        /* 128-byte frames: (128/12 - 1) x MaxChainDepth 128 + 1 = 1153 pages, i.e. 4.5 MiB (mps.c) */
        CHECK(g.sc->maxio == (((128 / 12) - 1) * 128 + 1) * 4096);
        CHECK(g.MaxSegs() == (uint32_t)g.sc->maxio / 4096);   /* + 1, capped at the chain depth (D59) */
    }

    if (Want("D54")) {
        /* FreeBSD's mps_reinit(sc) from a callout or completion queues the reinit task (it sleeps) */
        Rig g;
        const int resets = g.ioc.diagResetCount;
        mps_lock(g.sc);
        mps_reinit(g.sc);                               /* as mps_periodic / mpssas_tm_timeout do */
        CHECK(g.ReinitQueued() && g.ioc.diagResetCount == resets);
        mps_unlock(g.sc);
        HostAdvance(5000);
        CHECK(!g.ReinitQueued() && g.ioc.diagResetCount == resets + 1 && !g.Gone());
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D78")) {
        /* queue(3) debugging is on in the host build: a broken link is caught, not walked */
        struct E { TAILQ_ENTRY(E) link; int v; };
        TAILQ_HEAD(, E) head = TAILQ_HEAD_INITIALIZER(head);
        E a{}, b{}, stray{};
        TAILQ_INSERT_TAIL(&head, &a, link);
        TAILQ_INSERT_TAIL(&head, &b, link);
        E *dummy = &stray;
        b.link.tqe_prev = &dummy;                       /* as a list misuse leaves it (FINAL L3) */
        gHost.panicsAllowed = 1;
        const int panics = gHost.panics;
        TAILQ_REMOVE(&head, &b, link);                  /* QMD_TAILQ_CHECK_PREV */
        CHECK(gHost.panics == panics + 1);
        gHost.panicsAllowed = 0;
        (void)stray;
    }

    if (Want("D18")) {
        /*
         * An "impossible" SGL state (no room left for a chain element):
         * FreeBSD panics; our panic() returns, so the IO must fail with
         * EFBIG instead of writing past its request frame.
         */
        Rig g;
        struct mps_softc *sc = g.sc;
        CHECK(g.MaxSegs() == (int)sc->max_io_pages + 1);     /* the cap the chain logic is built for */
        mps_lock(sc);
        struct mps_command *cm = mps_alloc_command(sc);
        CHECK(cm != nullptr);
        uint8_t *next = (uint8_t *)cm->cm_req + sc->reqframesz;   /* the next SMID's frame */
        uint8_t guard[32];
        memcpy(guard, next, sizeof(guard));
        cm->cm_sge = (MPI2_SGE_IO_UNION *)(next - 4);
        cm->cm_sglsize = 4;
        gHost.panicsAllowed = 2;
        const int panics = gHost.panics;
        const int rc = mps_add_dmaseg(cm, 0x10000, 4096, 0, 2);
        gHost.panicsAllowed = 0;
        CHECK(rc == EFBIG && gHost.panics == panics + 1);
        CHECK(memcmp(guard, next, sizeof(guard)) == 0);      /* nothing written past the frame */
        mps_free_command(sc, cm);
        mps_unlock(sc);
        /* the IO path still works */
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);
    }

    if (Want("D51")) {
        /* unload: the IOC completed an IO, its interrupt not yet delivered (ours is off before mps_free) */
        Rig g;
        Io *r = g.Read(0x0009, 0, 2);
        CHECK(g.Go(r) == kPosted);                      /* the reply is on the queue: no interrupt runs */
        CHECK(!r->done && g.ioc.InterruptPending());
        g.Detach();                                     /* mps_free */
        CHECK(r->done == 1 && r->status == CAM_REQ_CMP);   /* its real status, not DEV_NOT_THERE */
    }

    if (Want("D20b")) {
        /* READ/WRITE killed by our own reset: re-sent (CAM requeues it uncounted), bounded */
        Rig g;
        MockIocModel &m = g.ioc;
        m.holdIo = true;
        Io *r = g.Read(0x0009, 0, 4);
        CHECK(g.Go(r) == kPosted);
        m.holdIo = false;
        RequestReinit(g, "test");
        HostAdvance(2000);
        g.Pump();
        CHECK(r->done && r->status == CAM_REQ_CMP);             /* no SAM retry spent */
        m.holdIo = true;
        Io *r2 = g.Read(0x0009, 0, 5);
        CHECK(g.Go(r2) == kPosted);
        for (int i = 0; i < 4 && !r2->done; i++) {
            RequestReinit(g, "test");
            HostAdvance(2000);
            g.Pump();
        }
        CHECK(r2->done && r2->status == CAM_SCSI_BUS_RESET);    /* the 4th reset in a row goes up */
        m.holdIo = false;
    }

    if (Want("D20c")) {
        /* the firmware says the device is not there: DEV_NOT_THERE, and the OS rechecks the target now */
        Rig g;
        MockIocModel &m = g.ioc;
        const target_id_t t = g.Tid(0x0009);
        const int scans = g.Scans(t);
        m.notThereDev = 0x0009;
        Io *r = g.Read(0x0009, 0, 4);
        CHECK(g.Go(r) == kPosted);
        g.Pump();
        CHECK(r->done && r->status == CAM_DEV_NOT_THERE);
        CHECK(g.Scans(t) == scans + 1);
        m.notThereDev = 0;
    }

    /* D8: a SATA IDENTIFY that fails in a reinit's rediscovery (spin-up after a wake) */
    auto d8rig = [](Rig &r) {
        r.ioc.sataSerial[0x0009] = "ZR50AAAA";
        r.ioc.sataSerial[0x000B] = "ZR50BBBB";
    };
    /* IO from the OS while the identity is unconfirmed (the event task sleeps in an IDENTIFY) */
    struct D8Io { Io *io = nullptr; St st = kDone; int sentBefore = 0; };
    auto d8io = [](Rig &g, D8Io &d, target_id_t t) {
        gHost.onOtherThreads = [&g, &d, t]() {
            if (d.io || !g.sc->xnu.tids[t].unverified) return;
            d.sentBefore = g.ioc.ioCount[0x000B];
            d.io = g.Read(0x000B, 0, 3);
            d.st = g.Go(d.io);
        };
    };
    if (Want("D8")) {
        /* the same disk answers later: same id and generation, its IO held, then released */
        Rig g({0x0009, 0x000B}, d8rig);
        MockIocModel &m = g.ioc;
        const target_id_t tb = g.Tid(0x000B), t9 = g.Tid(0x0009);
        const uint32_t gb = Gen(g, tb), g9 = Gen(g, t9);
        D8Io d;
        d8io(g, d, tb);
        m.identifyFailN[0x000B] = 5 + 2;            /* the add's 5 tries, then 2 of the first re-IDENTIFY */
        RequestReinit(g, "wake");
        HostAdvance(60000);
        gHost.onOtherThreads = nullptr;
        CHECK(d.io && d.st == kHeld);               /* held while the identity was open */
        g.Pump();
        CHECK(d.io->done && d.io->status == CAM_REQ_CMP);
        CHECK(g.Tid(0x000B) == tb && Gen(g, tb) == gb);        /* the OS kept the nub */
        CHECK(g.Tid(0x0009) == t9 && Gen(g, t9) == g9);
        CHECK(NoneUnverified(g.sc) && g.HeldCount() == 0);
        Io *r2 = g.Read(0x000B, 0, 5);
        CHECK(g.Go(r2) == kPosted);
        g.Pump();
        CHECK(r2->done && r2->status == CAM_REQ_CMP);
    }

    if (Want("D8-swap")) {
        /* another disk in the bay across the sleep: no IO of the old one reaches it */
        Rig g({0x0009, 0x000B}, d8rig);
        MockIocModel &m = g.ioc;
        const target_id_t tb = g.Tid(0x000B);
        const uint32_t gb = Gen(g, tb);
        D8Io d;
        d8io(g, d, tb);
        m.identifyFailN[0x000B] = 5;
        m.sataSerial[0x000B] = "ZR50CCCC";
        RequestReinit(g, "wake");
        HostAdvance(60000);
        gHost.onOtherThreads = nullptr;
        g.Pump();
        CHECK(d.io && d.st == kHeld);
        CHECK(d.io->done && d.io->status == CAM_DEV_NOT_THERE);     /* failed, never sent */
        CHECK(m.ioCount[0x000B] == d.sentBefore);
        CHECK(Gen(g, tb) != gb);                                    /* the OS replaces the nub */
        CHECK(NoneUnverified(g.sc) && g.HeldCount() == 0);
    }

    if (Want("D8-giveup")) {
        /* no answer through the whole re-add schedule: held IO failed, a new device */
        Rig g({0x0009, 0x000B}, d8rig);
        MockIocModel &m = g.ioc;
        const target_id_t tb = g.Tid(0x000B);
        const uint32_t gb = Gen(g, tb);
        D8Io d;
        d8io(g, d, tb);
        m.identifyFail.insert(0x000B);
        RequestReinit(g, "wake");
        HostAdvance(400000);
        gHost.onOtherThreads = nullptr;
        g.Pump();
        CHECK(d.io && d.st == kHeld);
        CHECK(d.io->done && d.io->status == CAM_DEV_NOT_THERE);
        CHECK(m.ioCount[0x000B] == d.sentBefore);
        CHECK(Gen(g, tb) != gb);
        CHECK(NoneUnverified(g.sc) && g.HeldCount() == 0);
    }

    if (Want("D22")) {
        /* the default is FreeBSD's info|fault; error is opt-in (levels error, warn, debug); the
         * port's periph stand-ins (failed IO, surprise removal) are tagged info|error */
        CHECK(mps_xnu_level_mask(MPS_LEVEL_INFO) == (MPS_INFO | MPS_FAULT));
        CHECK(mps_xnu_level_mask(MPS_LEVEL_ERROR) == (MPS_FAULT | MPS_ERROR));
        CHECK(mps_xnu_level_mask(MPS_LEVEL_WARN) == (MPS_FAULT | MPS_ERROR | MPS_RECOVERY));
        CHECK((mps_xnu_level_mask(MPS_LEVEL_DEBUG) & (MPS_INFO | MPS_FAULT | MPS_ERROR)) == (MPS_INFO | MPS_FAULT | MPS_ERROR));
        CHECK(mps_xnu_log_type(MPS_INFO | MPS_ERROR) == MPS_XNU_LOG_ERROR);
        CHECK(mps_xnu_log_type(MPS_XINFO | MPS_RECOVERY) == MPS_XNU_LOG_DEBUG);
        CHECK(mps_xnu_log_type(MPS_INIT | MPS_XINFO) == MPS_XNU_LOG_DEBUG);
        CHECK(mps_xnu_log_type(MPS_RECOVERY | MPS_INFO) == MPS_XNU_LOG_DEFAULT);
        size_t mark = LogMark();
        Rig g({0x0009, 0x000A}, [](Rig &r) { mps_get_tunables(r.sc); });     /* the core's default mask */
        CHECK(g.sc->mps_debug == (MPS_INFO | MPS_FAULT));
        /* attach and discovery: no event field dumps, no doorbell polls, no per-PHY lines */
        CHECK(!HasLog(mark, "Doorbell=") && !HasLog(mark, "PHY[0]"));
        CHECK(!HasLog(mark, "SasTopologyChangeList") && !HasLog(mark, "asks for an ack"));    /* no ack line (D29) */
        CHECK(HasLog(mark, "Firmware: "));                                   /* FreeBSD's info lines stay */
        mark = LogMark();
        CHECK(g.ForceReinit() == 0);
        CHECK(HasLog(mark, "Reinitializing controller") && !HasLog(mark, "Doorbell=") &&
              !HasLog(mark, "NULL handle for target"));
        for (size_t i = mark; i < gHost.log.size(); i++)
            CHECK(mps_xnu_log_type(gHost.log[i].cls) != MPS_XNU_LOG_DEBUG);  /* only what persists is printed */
        /* at the default mask: a MEDIUM ERROR IO and a surprise removal still print, as ERROR */
        {
            Rig g2({0x0009}, [](Rig &r) { mps_get_tunables(r.sc); });
            mark = LogMark();
            g2.ioc.failDev = 0x0009;
            Io *io = g2.Read(0x0009, 0, 7);
            CHECK(g2.Go(io) == kPosted);
            g2.Pump();
            CHECK(io->status == CAM_REQ_CMP_ERR);
            CHECK(HasLog(mark, " READ(10) lba 7 blocks 8 failed: IOCStatus scsi io data error"));
            CHECK(mps_xnu_log_type(LogClass(mark, "READ(10) lba 7 blocks 8 failed")) == MPS_XNU_LOG_ERROR);
            mark = LogMark();
            g2.ioc.dead = true;
            HostAdvance(1300);
            CHECK(g2.Gone() && HasLog(mark, "controller gone (surprise removal)") &&
                  HasLog(mark, "surprise removal: controller disabled"));
            CHECK(mps_xnu_log_type(LogClass(mark, "controller gone (surprise removal)")) == MPS_XNU_LOG_ERROR);
        }
    }

    if (Want("D24")) {
        /* MPS_FAULT is os_log FAULT; type and category come from the same class */
        CHECK(mps_xnu_log_type(MPS_FAULT) == MPS_XNU_LOG_FAULT && mps_xnu_log_type(MPS_INIT | MPS_FAULT) == MPS_XNU_LOG_FAULT);
        CHECK(mps_xnu_log_type(MPS_ERROR | MPS_RECOVERY) == MPS_XNU_LOG_ERROR);
        CHECK(strcmp(mps_xnu_category(MPS_ERROR | MPS_RECOVERY), "error") == 0);
        CHECK(strcmp(mps_xnu_category(MPS_XINFO | MPS_RECOVERY), "xinfo") == 0);
        for (uint32_t cls = 1; cls <= MPS_DEBUG_ALL; cls++) {
            const char *cat = mps_xnu_category(cls);
            uint32_t one = 0;
            for (uint32_t b = 1; b <= MPS_DEBUG_ALL; b <<= 1)
                if ((cls & b) && strcmp(mps_xnu_category(b), cat) == 0) one = b;
            CHECK(one != 0 && mps_xnu_log_type(cls) == mps_xnu_log_type(one));
        }
        /* an IOC fault and a failed reinit: FAULT lines */
        {
            Rig g({0x0009});
            size_t mark = LogMark();
            g.ioc.Fault(0x1234);
            g.ioc.nowUs += 1100000;
            g.Callouts();
            CHECK(mps_xnu_log_type(LogClass(mark, "IOC Fault 0x")) == MPS_XNU_LOG_FAULT);
            g.ioc.diagStuck = true;
            mark = LogMark();
            CHECK(g.ForceReinit() != 0 && g.Gone());
            CHECK(mps_xnu_log_type(LogClass(mark, "reinit failed (error")) == MPS_XNU_LOG_FAULT);
        }
        /* a surprise removal (an unplugged dock): ERROR, no FAULT line */
        {
            Rig g({0x0009});
            size_t mark = LogMark();
            g.ioc.dead = true;
            HostAdvance(1300);
            CHECK(g.Gone() && HasLog(mark, "controller gone (surprise removal)"));
            CHECK(mps_xnu_log_type(LogClass(mark, "controller gone (surprise removal)")) == MPS_XNU_LOG_ERROR);
            CHECK(mps_xnu_log_type(LogClass(mark, "surprise removal: controller disabled")) == MPS_XNU_LOG_ERROR);
            for (size_t i = mark; i < gHost.log.size(); i++)
                CHECK(mps_xnu_log_type(gHost.log[i].cls) != MPS_XNU_LOG_FAULT);
        }
    }

    if (Want("D26")) {
        /* the failed-IO limiter is per target: handles 0x09 and 0x11 (equal mod 8) do not share it */
        Rig g({0x0009, 0x0011});
        MockIocModel &m = g.ioc;
        m.nowUs += 11000000;
        size_t mark = LogMark();
        auto fail = [&](uint16_t h, int n) {
            m.failDev = h;
            for (int i = 0; i < n; i++) {
                Io *io = g.Read(h, 0, 7);
                CHECK(g.Go(io) == kPosted);
                g.Pump();
                CHECK(io->status == CAM_REQ_CMP_ERR);
            }
            m.failDev = 0;
        };
        fail(0x0009, 8);
        fail(0x0011, 7);
        char a[64], b[64];
        snprintf(a, sizeof(a), "target %u (handle 0x0009) lun 0 SMID", g.Tid(0x0009));
        snprintf(b, sizeof(b), "target %u (handle 0x0011) lun 0 SMID", g.Tid(0x0011));
        CHECK(CountLog(mark, a) == 5 && CountLog(mark, b) == 5);
        m.nowUs += 11000000;
        g.Callouts();                               /* mps_periodic: one exact summary per target */
        snprintf(a, sizeof(a), "target %u (handle 0x0009): 3 similar IO error lines suppressed", g.Tid(0x0009));
        snprintf(b, sizeof(b), "target %u (handle 0x0011): 2 similar IO error lines suppressed", g.Tid(0x0011));
        CHECK(HasLog(mark, a) && HasLog(mark, b));
        CHECK(CountLog(mark, "similar IO error lines suppressed") == 2);
    }

    if (Want("D49")) {
        /* sleep with an IOC that does not stop: suspended anyway, said so and counted; the wake recovers */
        Rig g;
        MockIocModel &m = g.ioc;
        m.murIgnored = true;
        size_t mark = LogMark();
        Suspend(g, 100);
        CHECK(g.Stats().suspend_stop_failed == 1 && (g.sc->mps_flags & MPS_FLAGS_XNU_SUSPENDED));
        CHECK(HasLog(mark, "suspended without a clean stop") && !HasLog(mark, "suspended: IOC stopped"));
        CHECK(mps_xnu_log_type(LogClass(mark, "suspended without a clean stop")) == MPS_XNU_LOG_ERROR);
        m.murIgnored = false;
        /* a clean sleep: not counted */
        Rig g2({0x0009});
        mark = LogMark();
        Suspend(g2, 100);
        CHECK(g2.Stats().suspend_stop_failed == 0 && HasLog(mark, "suspended: IOC stopped"));
    }
}
