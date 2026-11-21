/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_port_test.cpp - smoke test of the FreeBSD-shaped core (core/mps*.c,
 * core/compat) against the software IOC model (mock_ioc.h), with the host
 * mps_os_* hooks (mps_host_os.cpp).  Built with ASan/UBSan/LSan.
 *
 * Scenarios: attach (IOCFacts, IOCInit, queues, chain pool), port enable and
 * discovery of two targets through the CAM async callback, interrupts and
 * reply hardening, the thin CAM (held queue, requeue, device freeze),
 * IOC fault -> mps_periodic -> reinit task -> operational with rediscovery,
 * target removal, detach with no leaks.
 *
 * MOCK_VERBOSE=1 prints every log line.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "mps_host_os.h"

static int gChecks;
static void Fail(const char *file, int line, const char *expr);
#define CHECK(c) do { if (!(c)) Fail(__FILE__, __LINE__, #c); } while (0)
#define T(c) do { gChecks++; CHECK(c); } while (0)
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

/* ---- what the "OS" sees through the CAM async callback ---------------------- */

struct AsyncRec {
    uint32_t code;
    target_id_t tid;
    int fc;
};
static std::vector<AsyncRec> gAsync;

static void AsyncCb(void *arg, uint32_t code, struct cam_path *path, void *a)
{
    (void)arg;
    AsyncRec r{code, path ? path->target_id : CAM_TARGET_WILDCARD, -1};
    if (code == AC_XNU_SCAN)
        r.fc = ((union ccb *)a)->ccb_h.func_code;
    gAsync.push_back(r);
}

static int Scans(target_id_t tid)
{
    int n = 0;
    for (const AsyncRec &r : gAsync)
        if (r.code == AC_XNU_SCAN && r.tid == tid && r.fc == XPT_SCAN_TGT) n++;
    return n;
}

static int AsyncCount(uint32_t code)
{
    int n = 0;
    for (const AsyncRec &r : gAsync)
        if (r.code == code) n++;
    return n;
}

static bool Present(struct mps_softc *sc, u_int tid, uint16_t handle)
{
    uint16_t h = 0;
    return mps_xnu_target_present(sc, tid, &h) && h == handle;
}

template <class H> static int QueueLen(H *head)
{
    int n = 0;
    struct mps_command *cm;
    TAILQ_FOREACH(cm, head, cm_link) n++;
    return n;
}

/* ---- ccb completion ----------------------------------------------------------- */

static std::vector<std::pair<union ccb *, uint32_t>> gDone;

static void DoneCb(struct cam_periph *, union ccb *ccb)
{
    gDone.push_back({ccb, ccb->ccb_h.status});
}

static union ccb *NewIo(path_id_t pathid, target_id_t tid, lun_id_t lun, uintptr_t tag)
{
    union ccb *ccb = xpt_alloc_ccb();
    CHECK(xpt_create_path(&ccb->ccb_h.path, NULL, pathid, tid, lun) == CAM_REQ_CMP);
    xpt_setup_ccb(&ccb->ccb_h, ccb->ccb_h.path, CAM_PRIORITY_NORMAL);
    ccb->ccb_h.func_code = XPT_SCSI_IO;
    ccb->ccb_h.flags = CAM_DIR_NONE;
    ccb->ccb_h.cbfcnp = DoneCb;
    ccb->ccb_h.ppriv_ptr0 = (void *)tag;
    return ccb;
}

static void FreeIo(union ccb *ccb)
{
    xpt_free_path(ccb->ccb_h.path);
    xpt_free_ccb(ccb);
}

/* ---- scenarios ------------------------------------------------------------------- */

static MockIocModel gIoc;
static struct device gDev;

static struct mps_softc *Attach(std::vector<uint16_t> devices)
{
    gIoc.devices = devices;
    gIoc.maxTargets = 32;
    gIoc.strictEventMasks = false;          /* mpssas registers IR events too */
    gIoc.tmSmidLo = 1;                      /* high-priority SMIDs 1..num_prireqs */
    gIoc.tmSmidHi = 1 + MockIocModel::kHighPriorityCredit;
    gHost.ioc = &gIoc;
    gHost.verbose = getenv("MOCK_VERBOSE") != nullptr;

    struct mps_softc *sc = (struct mps_softc *)calloc(1, sizeof(*sc));
    gHost.sc = sc;
    mps_compat_device_init(&gDev, "mps", 0, sc, &gIoc, 0x0087);
    sc->mps_dev = &gDev;
    mps_get_tunables(sc);
    T(bus_dma_tag_create(bus_get_dma_tag(&gDev), 1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
                         NULL, NULL, BUS_SPACE_MAXSIZE_32BIT, BUS_SPACE_UNRESTRICTED,
                         BUS_SPACE_MAXSIZE_32BIT, 0, NULL, NULL, &sc->mps_parent_dmat) == 0);
    T(mps_attach(sc) == 0);
    return sc;
}

static void TestAttach(struct mps_softc *sc)
{
    printf("attach: IOCFacts, IOCInit, queues, chain pool\n");
    T(sc->mps_flags & MPS_FLAGS_ATTACH_DONE);
    T(gIoc.iocInits == 1);
    T(gIoc.state == MPI2_IOC_STATE_OPERATIONAL);
    T(gIoc.diagResetCount >= 1);            /* no EVENT_REPLAY capability: reset at attach */
    T(sc->facts->RequestCredit == 100);
    T(sc->num_prireqs == MockIocModel::kHighPriorityCredit);     /* min(MPS_PRI_REQ_FRAMES 128, 8) */
    /* RequestCredit 100 < MPS_REQ_FRAMES: 100 normal frames, 92 for the OS */
    T(sc->num_reqs == 100 + MockIocModel::kHighPriorityCredit);
    T(sc->xnu.io_cmds == 100 - MPS_XNU_INTERNAL_CMDS);
    T(sc->num_replies == 127);              /* min(108 + 32, 128) - 1 */
    T(sc->fqdepth == 128 && sc->pqdepth == 128);
    T(gIoc.fq == 128 && gIoc.pq == 128 && gIoc.frameSz == 128);
    T(sc->reqframesz == 128 && sc->replyframesz == 128);
    T(sc->maxio == MPS_XNU_MAXIO_PAGES * PAGE_SIZE);
    /* (256 pages + 9 - 2) / 9 SGEs per chain frame, per request credit */
    T(sc->num_chains == 29 * 100);
    T(sc->chain_free == sc->num_chains);
    T(QueueLen(&sc->req_list) == sc->num_reqs - sc->num_prireqs - 1);
    T(QueueLen(&sc->high_priority_req_list) == sc->num_prireqs);
    /* the chain pool is one allocation mapped as many 4 KiB segments */
    T(gHost.dmaMultiSeg >= 1 && gHost.maxSegs >= 65);
    int chains = 0;
    bool consistent = true;
    struct mps_chain *ch;
    TAILQ_FOREACH(ch, &sc->chain_list, chain_link) {
        chains++;
        consistent &= ch->chain_busaddr % 16 == 0 && ch->chain_busaddr <= 0xFFFFFFFFu;
        consistent &= gIoc.Ptr(ch->chain_busaddr, sc->reqframesz) == (uint8_t *)ch->chain;
    }
    T(chains == sc->num_chains && consistent);
    T(gIoc.Ptr(sc->req_busaddr, sc->reqframesz) == sc->req_frames);
    T(sc->req_busaddr == gIoc.reqBase && sc->post_busaddr == gIoc.postBase &&
      sc->free_busaddr == gIoc.freeBase);
    T(sc->sassc != NULL && sc->sassc->maxtargets == 32);
    T(gIoc.eventRegistrations == 2);        /* mps_attach_log + mpssas_register_events */
    T(!(gIoc.lastEventMasks[0] & (1u << MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST)));
    T(!(gIoc.lastEventMasks[0] & (1u << MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE)));
    T(gHost.interruptsSetUp == 1);
    T(callout_pending(&sc->periodic));
    T(sc->sassc->flags & MPSSAS_IN_STARTUP);
    T(sc->sassc->sim->freeze == 1);         /* held until discovery is done */
    T(gHost.Has(0, "Firmware: 20.00.00.00"));
}

static void TestDiscovery(struct mps_softc *sc)
{
    printf("port enable + discovery of 2 targets via the CAM async callback\n");
    struct cam_path *path;
    T(xpt_create_path(&path, NULL, cam_sim_path(sc->sassc->sim), CAM_TARGET_WILDCARD,
                      CAM_LUN_WILDCARD) == CAM_REQ_CMP);
    T(xpt_register_async(AC_XNU_SCAN | AC_BUS_RESET | AC_FOUND_DEVICE | AC_LOST_DEVICE,
                         AsyncCb, NULL, path) == CAM_REQ_CMP);
    xpt_free_path(path);

    size_t mark = gHost.Mark();
    mps_compat_intrhook_run();              /* mps_startup: unmask, port enable */
    T(gIoc.portEnables == 1);
    HostRunPending();                       /* interrupt, event task */
    T(gHost.interruptsDelivered > 0);
    T(sc->port_enable_complete == 1 && sc->wait_for_port_enable == 0);
    T(gIoc.ackCount == 1);                  /* topology event acknowledged (XNU) */
    T(Scans(0) == 1 && Scans(1) == 1);
    T(Present(sc, 0, 0x0009) && Present(sc, 1, 0x000A) && !mps_xnu_target_present(sc, 2, NULL));
    T(sc->sassc->targets[0].linkrate == 0xA && sc->sassc->targets[0].tid == 0);
    T(sc->sassc->startup_refcount == 0);
    T(!(sc->sassc->flags & MPSSAS_IN_STARTUP));
    T(sc->sassc->sim->freeze == 0);
    T(gHost.Count(mark, "Target id 0x") == 2);
    T(gIoc.hostPost == gIoc.iocPost);
}

static void TestInterrupts(struct mps_softc *sc)
{
    printf("interrupts: stale, bad SMID, bad reply address, masking\n");
    struct mps_xnu_stats &st = sc->xnu.stats;
    uint32_t stale = st.stale_replies, bad = st.bad_replies;
    MPI2_REPLY_DESCRIPTORS_UNION d{};

    d.SCSIIOSuccess.ReplyFlags = MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS;
    d.SCSIIOSuccess.SMID = 50;              /* idle command */
    gIoc.PostDescriptor(d);
    HostRunPending();
    T(st.stale_replies == stale + 1);

    d.SCSIIOSuccess.SMID = 0x7FFF;          /* no such command */
    gIoc.PostDescriptor(d);
    HostRunPending();
    T(st.bad_replies == bad + 1);

    MPI2_REPLY_DESCRIPTORS_UNION a{};
    a.AddressReply.ReplyFlags = MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY;
    a.AddressReply.SMID = 60;
    a.AddressReply.ReplyFrameAddress = (uint32_t)sc->reply_busaddr + 5;   /* not a frame */
    gIoc.PostDescriptor(a);
    HostRunPending();
    T(st.bad_replies == bad + 2);

    uint32_t hostFree = gIoc.hostFree;
    MPI2_DEFAULT_REPLY r{};
    r.MsgLength = sizeof(r) / 4;
    r.Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    gIoc.AddressReply(61, &r, sizeof(r));   /* a real frame for an idle SMID */
    HostRunPending();
    T(st.stale_replies == stale + 2);
    T(gIoc.hostFree != hostFree);           /* the frame went back to the free queue */

    mps_lock(sc);
    mps_mask_intr(sc);
    mps_unlock(sc);
    d.SCSIIOSuccess.SMID = 51;
    gIoc.PostDescriptor(d);
    HostRunPending();
    T(st.stale_replies == stale + 2);       /* masked: not delivered */
    mps_lock(sc);
    mps_unmask_intr(sc);
    mps_unlock(sc);
    HostRunPending();
    T(st.stale_replies == stale + 3);
    T(gIoc.hostPost == gIoc.iocPost);

    /* a ccb through the driver's SIM: held while frozen, then a TEST UNIT READY (CHECK CONDITION) */
    gDone.clear();
    union ccb *ccb = NewIo(cam_sim_path(sc->sassc->sim), 0, 0, 1);
    mps_lock(sc);
    xpt_freeze_simq(sc->sassc->sim, 1);
    xpt_action(ccb);
    T(gDone.empty() && cam_xnu_held_count(sc->sassc->sim) == 1);
    xpt_release_simq(sc->sassc->sim, 1);
    mps_unlock(sc);
    HostRunPending();
    T(gDone.size() == 1 && (gDone[0].second & CAM_STATUS_MASK) == CAM_SCSI_STATUS_ERROR);
    FreeIo(ccb);
}

/* ---- the thin CAM, with a SIM of the test's own ------------------------------- */

struct TestSim {
    std::vector<uintptr_t> actions;         /* tags, in dispatch order */
    std::vector<uintptr_t> requeueOnce;     /* tags to requeue (with a device freeze) once */
    std::vector<uintptr_t> requeueFrozen;   /* tags to requeue once, SIM queue frozen (no command) */
};

static void TestSimAction(struct cam_sim *sim, union ccb *ccb)
{
    TestSim *ts = (TestSim *)cam_sim_softc(sim);
    uintptr_t tag = (uintptr_t)ccb->ccb_h.ppriv_ptr0;
    ts->actions.push_back(tag);
    for (size_t i = 0; i < ts->requeueOnce.size(); i++) {
        if (ts->requeueOnce[i] == tag) {
            ts->requeueOnce.erase(ts->requeueOnce.begin() + i);
            xpt_freeze_devq(ccb->ccb_h.path, 1);
            ccb->ccb_h.status = CAM_REQUEUE_REQ | CAM_DEV_QFRZN;
            xpt_done(ccb);
            return;
        }
    }
    for (size_t i = 0; i < ts->requeueFrozen.size(); i++) {
        if (ts->requeueFrozen[i] == tag) {
            ts->requeueFrozen.erase(ts->requeueFrozen.begin() + i);
            xpt_freeze_simq(sim, 1);                /* mpssas_action_scsiio without a free command */
            ccb->ccb_h.status = CAM_REQUEUE_REQ;
            xpt_done(ccb);
            return;
        }
    }
    ccb->ccb_h.status = CAM_REQ_CMP;
    xpt_done(ccb);
}

static size_t DoneIndex(uintptr_t tag)
{
    for (size_t i = 0; i < gDone.size(); i++)
        if ((uintptr_t)gDone[i].first->ccb_h.ppriv_ptr0 == tag) return i;
    return (size_t)-1;
}

static void TestThinCam()
{
    printf("thin CAM: frozen SIM queue, requeue, device freeze, async\n");
    TestSim ts;
    struct mtx m;
    mtx_init(&m, "tsim", NULL, MTX_DEF);
    struct cam_devq *devq = cam_simq_alloc(8);
    struct cam_sim *sim = cam_sim_alloc(TestSimAction, NULL, "tsim", &ts, 0, &m, 8, 8, devq);
    T(sim != NULL && xpt_bus_register(sim, NULL, 0) == 0);
    path_id_t pid = cam_sim_path(sim);
    std::vector<union ccb *> ccbs;
    auto io = [&](target_id_t t, uintptr_t tag) {
        union ccb *c = NewIo(pid, t, 0, tag);
        ccbs.push_back(c);
        xpt_action(c);
    };
    gDone.clear();
    mtx_lock(&m);

    /* frozen SIM queue: held in arrival order, dispatched on release */
    xpt_freeze_simq(sim, 1);
    io(1, 1); io(1, 2); io(2, 3);
    T(ts.actions.empty() && cam_xnu_held_count(sim) == 3);
    xpt_release_simq(sim, 1);
    T((ts.actions == std::vector<uintptr_t>{1, 2, 3}) && gDone.size() == 3);

    /* requeue with CAM_DEV_QFRZN: held again, freeze released, retried when the pass ends (D35) */
    ts.actions.clear();
    ts.requeueOnce = {4};
    io(2, 4);
    T((ts.actions == std::vector<uintptr_t>{4}) && cam_xnu_held_count(sim) == 1);
    mtx_unlock(&m);                         /* the SIM lets its lock go: the held queue runs once */
    mtx_lock(&m);
    T((ts.actions == std::vector<uintptr_t>{4, 4}));
    T(DoneIndex(4) != (size_t)-1 && (gDone[DoneIndex(4)].second & CAM_STATUS_MASK) == CAM_REQ_CMP);
    T(TAILQ_EMPTY(&sim->frozen) && cam_xnu_held_count(sim) == 0);

    /* device freeze: only that device's IO waits */
    ts.actions.clear();
    struct cam_path *p3;
    T(xpt_create_path(&p3, NULL, pid, 3, CAM_LUN_WILDCARD) == CAM_REQ_CMP);
    T(xpt_freeze_devq(p3, 1) == 1);
    io(3, 5); io(4, 6); io(3, 7);
    T((ts.actions == std::vector<uintptr_t>{6}) && cam_xnu_held_count(sim) == 2);
    xpt_release_devq(p3, 1, TRUE);
    T((ts.actions == std::vector<uintptr_t>{6, 5, 7}) && cam_xnu_held_count(sim) == 0);
    xpt_free_path(p3);

    /* a requeued ccb keeps its place ahead of later arrivals; a frozen SIM stops the run at once */
    ts.actions.clear();
    ts.requeueFrozen = {8};
    xpt_freeze_simq(sim, 1);
    io(5, 8); io(6, 9);
    xpt_release_simq(sim, 1);
    T((ts.actions == std::vector<uintptr_t>{8}) && cam_xnu_held_count(sim) == 2 && sim->freeze == 1);
    xpt_release_simq(sim, 1);
    T(DoneIndex(8) != (size_t)-1 && DoneIndex(9) != (size_t)-1);
    T((ts.actions == std::vector<uintptr_t>{8, 8, 9}) && cam_xnu_held_count(sim) == 0 && sim->freeze == 0);

    /* async callbacks and deregistration */
    gAsync.clear();
    struct cam_path *pw;
    T(xpt_create_path(&pw, NULL, pid, CAM_TARGET_WILDCARD, CAM_LUN_WILDCARD) == CAM_REQ_CMP);
    T(xpt_register_async(AC_BUS_RESET, AsyncCb, &ts, pw) == CAM_REQ_CMP);
    xpt_async(AC_BUS_RESET, pw, NULL);
    xpt_async(AC_LOST_DEVICE, pw, NULL);    /* not registered for */
    T(AsyncCount(AC_BUS_RESET) == 1 && AsyncCount(AC_LOST_DEVICE) == 0);
    T(xpt_register_async(0, AsyncCb, &ts, pw) == CAM_REQ_CMP);
    xpt_async(AC_BUS_RESET, pw, NULL);
    T(AsyncCount(AC_BUS_RESET) == 1);
    xpt_free_path(pw);

    /* held ccbs are flushed when the SIM goes away */
    xpt_freeze_simq(sim, 1);
    io(7, 10);
    size_t n = gDone.size();
    xpt_bus_deregister(pid);
    cam_sim_free(sim, TRUE);
    T(gDone.size() == n + 1 && (gDone.back().second & CAM_STATUS_MASK) == CAM_DEV_NOT_THERE);
    mtx_unlock(&m);
    mtx_destroy(&m);
    for (union ccb *c : ccbs) FreeIo(c);
    gAsync.clear();
}

static void TestFaultReinit(struct mps_softc *sc)
{
    printf("IOC fault -> mps_periodic -> reinit task -> operational, rediscovery\n");
    gAsync.clear();
    size_t mark = gHost.Mark();
    int diag0 = gIoc.diagResetCount, acks0 = gIoc.ackCount;
    bool sawReset = false;
    gHost.onOtherThreads = [&] { if (sc->mps_flags & MPS_FLAGS_DIAGRESET) sawReset = true; };
    gIoc.Fault(0x2622);
    HostAdvance(1100);                      /* the 1 s watchdog sees the FAULT */
    gHost.onOtherThreads = nullptr;
    T(sc->xnu.stats.faults == 1 && sc->xnu.stats.reinits == 1 && sc->xnu.stats.reinit_failures == 0);
    T(sawReset);                            /* other threads ran while the reinit waited */
    T(gIoc.diagResetCount == diag0 + 1 && gIoc.iocInits == 2);
    T(gHost.pciRestores == 1);
    T(gIoc.state == MPI2_IOC_STATE_OPERATIONAL);
    T(!(sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE)));
    T(AsyncCount(AC_BUS_RESET) == 1);       /* mpssas_announce_reset */
    T(gIoc.eventRegistrations == 3 && gIoc.portEnables == 2);
    T(Scans(0) == 1 && Scans(1) == 1);
    T(Present(sc, 0, 0x0009) && Present(sc, 1, 0x000A));   /* same target ids */
    T(gIoc.ackCount == acks0 + 1);
    T(sc->sassc->startup_refcount == 0 && !(sc->sassc->flags & MPSSAS_IN_STARTUP));
    T(sc->sassc->sim->freeze == 0);
    T(QueueLen(&sc->req_list) == sc->num_reqs - sc->num_prireqs - 1);
    T(gHost.Has(mark, "IOC Fault 0x40002622") && gHost.Has(mark, "reinit done"));
    T(callout_pending(&sc->periodic));
}

static void TestReinitRetry(struct mps_softc *sc)
{
    printf("reinit: first diag reset times out, second attempt succeeds\n");
    gAsync.clear();
    uint32_t reinits = sc->xnu.stats.reinits, fails = sc->xnu.stats.reinit_failures;
    uint64_t t0 = gIoc.nowUs;
    gIoc.diagStuck = true;                  /* RESET_ADAPTER never clears ... */
    gHost.onOtherThreads = [&] {            /* ... until 400 s later */
        if (gIoc.diagStuck && gIoc.nowUs > t0 + 400000000ull) {
            gIoc.diagStuck = false;
            gIoc.state = MPI2_IOC_STATE_READY;
        }
    };
    gIoc.Fault(0x2623);
    HostAdvance(1100);
    gHost.onOtherThreads = nullptr;
    T(sc->xnu.stats.reinits == reinits + 1 && sc->xnu.stats.reinit_failures == fails + 1);
    T(!(sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE)) && gHost.dead == 0);
    T(gIoc.state == MPI2_IOC_STATE_OPERATIONAL);
    T(Present(sc, 0, 0x0009) && Present(sc, 1, 0x000A));
    T(sc->sassc->sim->freeze == 0 && gHost.pciRestores == 3);   /* 1 + both attempts here (D19) */
}

static void TestRemoval(struct mps_softc *sc)
{
    printf("removal: TARG_NOT_RESPONDING -> target reset -> REMOVE_DEVICE\n");
    gAsync.clear();
    gIoc.tms.clear();
    int acks0 = gIoc.ackCount;
    gIoc.PostTopologyEvent({0x000A}, MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING);
    HostRunPending();
    T(gIoc.ackCount == acks0 + 1);
    T(gIoc.tms.size() == 1 && gIoc.tms[0].type == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET &&
      gIoc.tms[0].msgFlags == MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET && gIoc.tms[0].dev == 0x000A &&
      gIoc.tms[0].descFlags == MPI2_REQ_DESCRIPT_FLAGS_HIGH_PRIORITY);
    T(gIoc.removeDevices == 1);
    T(Scans(1) == 1);
    T(!mps_xnu_target_present(sc, 1, NULL) && sc->sassc->targets[1].handle == 0);
    T(Present(sc, 0, 0x0009));
    T(TAILQ_EMPTY(&sc->sassc->sim->frozen));                       /* devq freeze released */
    T(QueueLen(&sc->high_priority_req_list) == sc->num_prireqs);   /* TM returned */
}

static void TestDetach(struct mps_softc *sc)
{
    printf("detach: no DMA or memory leaks\n");
    int mur0 = gIoc.murCount;
    T(mps_free(sc) == 0);
    T(bus_dma_tag_destroy(sc->mps_parent_dmat) == 0);
    T(gIoc.murCount == mur0 + 1);           /* IOC back to READY */
    T(gHost.interruptsFreed == 1);
    T(mps_compat_dmamem_count() == 0);
    T(gIoc.liveAllocs == 0 && gIoc.regions.empty());
    T(mps_compat_malloc_count() == 0);
    T(gHost.armed.empty() && gHost.queues.empty() && gHost.locksHeld == 0);
    free(sc);
    gHost.sc = nullptr;
}

static void TestXnuHelpers(struct mps_softc *sc)
{
    printf("xnu helpers: levels, mps_parse_debug, %%b, decoders, rate limit\n");
    T(mps_xnu_level_mask(MPS_LEVEL_ERROR) == (MPS_FAULT | MPS_ERROR));
    T(mps_xnu_level_mask(MPS_LEVEL_INFO) ==
      (MPS_FAULT | MPS_ERROR | MPS_RECOVERY | MPS_INFO | MPS_INIT | MPS_EVENT | MPS_MAPPING));
    T(mps_xnu_level_mask(MPS_LEVEL_TRACE) == MPS_DEBUG_ALL);
    T(mps_xnu_parse_level("debug") == 3 && mps_xnu_parse_level("4") == 4 &&
      mps_xnu_parse_level("warning") == 1 && mps_xnu_parse_level("loud") == -1);
    T(strcmp(mps_xnu_category(MPS_RECOVERY | MPS_INFO), "recovery") == 0);
    T(strcmp(mps_xnu_category(MPS_INIT | MPS_FAULT), "fault") == 0);
    uint32_t saved = sc->mps_debug;
    char l1[] = "+trace,0x100";
    mps_parse_debug(sc, l1);
    T(sc->mps_debug == (saved | MPS_TRACE | MPS_USER));
    char l2[] = "info,fault";
    mps_parse_debug(sc, l2);
    T(sc->mps_debug == (MPS_INFO | MPS_FAULT));
    sc->mps_debug = saved;
    char b[160];
    mps_compat_snprintf(b, sizeof(b), "%b", 0x5, "\20\1InProgress\2DeviceChange\3Third");
    T(strcmp(b, "5<InProgress,Third>") == 0);
    T(strcmp(mps_xnu_cdb_str(0x28), "READ(10)") == 0 && strcmp(mps_xnu_cdb_str(0xEE), "CDB") == 0);
    uint8_t sense[18] = {0x70, 0, 0x03, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x11, 0x00};
    uint8_t key, asc, ascq;
    T(mps_xnu_parse_sense(sense, sizeof(sense), &key, &asc, &ascq) && key == 3 && asc == 0x11);
    T(strcmp(mps_xnu_asc_str(asc, ascq), "UNRECOVERED READ ERROR") == 0);
    T(strcmp(mps_xnu_sense_key_str(key), "MEDIUM ERROR") == 0);
    mps_xnu_loginfo_str(0x31120303, b, sizeof(b));
    T(strcmp(b, "0x31120303 (SAS PL code 0x12 sub 0x0303)") == 0);
    uint64_t lba;
    uint32_t blocks;
    const uint8_t cdb[10] = {0x28, 0, 0, 0, 0x10, 0x00, 0, 0, 8, 0};
    T(mps_xnu_cdb_lba(cdb, 10, &lba, &blocks) && lba == 0x1000 && blocks == 8);
    struct mps_xnu_ratelimit rl = {};
    uint32_t sup, allowed = 0;
    for (int i = 0; i < 8; i++)
        allowed += mps_xnu_ratelimit_allow(&rl, 1000, MPS_XNU_RL_BURST, MPS_XNU_RL_WINDOW_MS, &sup);
    T(allowed == MPS_XNU_RL_BURST && rl.suppressed == 3);
    T(mps_xnu_ratelimit_flush(&rl, 1000 + MPS_XNU_RL_WINDOW_MS, MPS_XNU_RL_WINDOW_MS, &sup) && sup == 3);
}

static void TestGone()
{
    printf("second attach; surprise removal (all-ones) -> gone, detach frees everything\n");
    struct mps_softc *sc = Attach({0x0011});
    gAsync.clear();
    struct cam_path *path;
    T(xpt_create_path(&path, NULL, cam_sim_path(sc->sassc->sim), CAM_TARGET_WILDCARD,
                      CAM_LUN_WILDCARD) == CAM_REQ_CMP);
    T(xpt_register_async(AC_XNU_SCAN | AC_BUS_RESET, AsyncCb, NULL, path) == CAM_REQ_CMP);
    xpt_free_path(path);
    mps_compat_intrhook_run();
    HostRunPending();
    T(Present(sc, 0, 0x0011) && Scans(0) == 1);
    size_t mark = gHost.Mark();
    gIoc.dead = true;                       /* Thunderbolt cable pulled */
    HostAdvance(1100);
    T(sc->mps_flags & MPS_FLAGS_XNU_GONE);
    T(!mps_xnu_target_present(sc, 0, NULL));
    T(gHost.Count(mark, "controller gone") == 1);
    T(sc->xnu.stats.reinits == 0);          /* nothing to reset */
    int frees = gHost.interruptsFreed;
    T(mps_free(sc) == 0);
    T(bus_dma_tag_destroy(sc->mps_parent_dmat) == 0);
    T(gHost.interruptsFreed == frees + 1);
    T(mps_compat_dmamem_count() == 0 && gIoc.liveAllocs == 0 && mps_compat_malloc_count() == 0);
    T(gHost.armed.empty() && gHost.queues.empty() && gHost.locksHeld == 0);
    free(sc);
    gHost.sc = nullptr;
    gIoc.dead = false;
}

int main()
{
    struct mps_softc *sc = Attach({0x0009, 0x000A});
    TestAttach(sc);
    TestDiscovery(sc);
    TestInterrupts(sc);
    TestThinCam();
    TestXnuHelpers(sc);
    TestFaultReinit(sc);
    TestReinitRetry(sc);
    TestRemoval(sc);
    TestDetach(sc);
    TestGone();
    printf("PASS (%d checks, %zu log lines)\n", gChecks, gHost.log.size());
    return 0;
}
