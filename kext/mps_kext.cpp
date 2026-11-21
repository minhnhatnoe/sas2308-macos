/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_kext.cpp - the mps_kext_*() calls of mps_xnu_os.h: the IOKit class's
 * view of the FreeBSD-shaped core (FreeBSD mps_pci.c's role): the call
 * sequence of test/mock_ioc_test.cpp `Rig', ccbs for SCSI IO and task
 * management, completions queued for the class to deliver without mps_mtx
 * (IOSCSIParallelFamily must not be called with it held).
 *
 * Portable: no IOKit, only the core and the mps_os_*() hooks, so the host
 * tests compile and run it unchanged (test/kext_glue_test.cpp) against
 * test/mps_host_os.cpp; the kext builds it with mps_xnu_os.cpp's hooks.
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wunknown-warning-option"
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
              MPSK_DEBUG_ALL == MPS_DEBUG_ALL, "debug classes");
static_assert(MPSK_DEVINFO_SSP_TARGET == MPI2_SAS_DEVICE_INFO_SSP_TARGET &&
              MPSK_DEVINFO_STP_TARGET == MPI2_SAS_DEVICE_INFO_STP_TARGET &&
              MPSK_DEVINFO_SATA_DEVICE == MPI2_SAS_DEVICE_INFO_SATA_DEVICE, "device info bits");
static_assert(MPSK_LEVEL_INFO == MPS_LEVEL_INFO && MPSK_LEVEL_TRACE == MPS_LEVEL_TRACE, "levels");
static_assert(MPSK_MAX_TARGETS == MPS_XNU_MAX_TARGETS, "sizes");
static_assert(sizeof(struct mps_kext_seg) == sizeof(bus_dma_segment_t) &&
              __builtin_offsetof(struct mps_kext_seg, addr) == __builtin_offsetof(bus_dma_segment_t, ds_addr) &&
              __builtin_offsetof(struct mps_kext_seg, len) == __builtin_offsetof(bus_dma_segment_t, ds_len),
              "segment layout");

static inline void memset_zero(void *p, size_t n) { memset(p, 0, n); }

/* ======================================================================
 * mps_kext_*: the IOKit class's view of the core
 * ==================================================================== */

#define MPSK_SEG_BYTES		0xFFFFFFull			/* MPI2 simple SGE length */
#define MPSK_INTERNAL_CMDS	8	/* normal frames kept from the OS (mps_kext_max_tasks) */

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
    int                     ua_retried; /* re-sent once for a UNIT ATTENTION after our reset (D3) */
    struct mps_kext_task_glue glue;     /* the glue's (D45) */
    /* then mps_kext_max_segs() bus_dma_segment_t (mps_kext_task_size) */
};

static inline bus_dma_segment_t *mps_kext_task_seglist(struct mps_kext_task *t)
{
    return (bus_dma_segment_t *)(void *)(t + 1);
}

/* An OS target reset (XPT_RESET_DEV, D10). */
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
    int                     drain_chan;     /* mps_kext_target_drain */
    void                   *inline_owner;   /* this thread drains the done queue itself */
    void                   *done_lock;      /* leaf lock for the done queue (mps_os_lock) */
    struct mps_kext_task   *done_head, *done_tail;
    /* CAM da's DA_FLAG_RETRY_UA (D3): per target, the LUNs (bit 63: 63 and up) armed by our reset */
    uint64_t                ua_luns[MPSK_MAX_TARGETS];
    uint32_t                ua_gen[MPSK_MAX_TARGETS];   /* the target's generation when armed */
    struct callout          hold_callout;   /* debug: SIM queue frozen for a while */
    int                     hold_active;
};

static volatile int gKexts = 0;

static void mps_kext_result(struct mps_kext_task *t, struct mps_kext_io_result *res)
{
    struct ccb_scsiio *csio = &t->ccb.csio;
    const uint32_t st = t->ccb.ccb_h.status & CAM_STATUS_MASK;

    memset_zero(res, sizeof(*res));
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
    case CAM_AUTOSENSE_FAIL:
        /*
         * CHECK CONDITION whose sense could not be fetched: the device's
         * answer, without sense (cam_periph_error: EIO, no retry; D20a).
         */
        res->kind = MPSK_IO_STATUS;
        res->scsi_status = MPI2_SCSI_STATUS_CHECK_CONDITION;
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

/*
 * CAM da's RETRY_UA (scsi_da.c daasync: AC_SENT_BDR / AC_BUS_RESET set
 * DA_FLAG_RETRY_UA, dadone passes SF_RETRY_UA): the UNIT ATTENTION a drive
 * returns after a reset of ours (reinit, wake, target or LUN reset) is not
 * the OS's business. SAM sends SYNCHRONIZE CACHE once and fails it on
 * anything but GOOD (IOSCSIBlockCommandsDevice::SynchronizeCache), and
 * absorbs 29/00 only at power-on, so we re-send such a ccb, once per ccb
 * (like da's DA_CCB_RETRY_UA on every pending ccb), uncounted by SAM, while
 * its LUN is armed; the first GOOD from the LUN disarms it, and so does a
 * different disk behind the target id (generation). da retries any sense
 * key 6 while armed; we match ASC 0x29 (POWER ON, RESET, OR BUS DEVICE
 * RESET OCCURRED) only, the UA our reset causes. mps_mtx held.
 */
static void mps_kext_arm_ua(struct mps_kext *k, target_id_t tid, lun_id_t lun)
{
    const uint64_t luns = lun == CAM_LUN_WILDCARD ? ~0ull : 1ull << (lun < 63 ? lun : 63);

    for (u_int i = 0; i < MPSK_MAX_TARGETS; i++) {
        if (tid != CAM_TARGET_WILDCARD && i != tid)
            continue;
        if (k->ua_luns[i] == 0 || k->ua_gen[i] != mps_xnu_target_gen(&k->sc, i)) {
            k->ua_luns[i] = 0;
            k->ua_gen[i] = mps_xnu_target_gen(&k->sc, i);
        }
        k->ua_luns[i] |= luns;
    }
}

static int mps_kext_retry_ua(struct mps_kext *k, struct mps_kext_task *t)
{
    union ccb *ccb = &t->ccb;
    struct ccb_scsiio *csio = &ccb->csio;
    const target_id_t tid = ccb->ccb_h.path ? ccb->ccb_h.path->target_id : ccb->ccb_h.target_id;
    const lun_id_t lun = ccb->ccb_h.path ? ccb->ccb_h.path->lun_id : ccb->ccb_h.target_lun;
    const uint64_t bit = 1ull << (lun < 63 ? lun : 63);
    const uint32_t st = ccb->ccb_h.status & CAM_STATUS_MASK;
    const uint8_t *sense = (const uint8_t *)&csio->sense_data;
    uint8_t key, asc, ascq;

    if (tid >= MPSK_MAX_TARGETS || (k->ua_luns[tid] & bit) == 0)
        return 0;
    if (k->ua_gen[tid] != mps_xnu_target_gen(&k->sc, tid)) {
        k->ua_luns[tid] = 0;                    /* another disk now */
        return 0;
    }
    if (st == CAM_REQ_CMP) {
        k->ua_luns[tid] &= ~bit;                /* the LUN is past its UA */
        return 0;
    }
    if (st != CAM_SCSI_STATUS_ERROR || csio->scsi_status != MPI2_SCSI_STATUS_CHECK_CONDITION || t->ua_retried ||
        (ccb->ccb_h.status & CAM_AUTOSNS_VALID) == 0 || csio->sense_len - csio->sense_resid < 14)
        return 0;
    if ((sense[0] & 0x7F) >= 0x72) {            /* descriptor format */
        key = sense[1] & 0x0F; asc = sense[2]; ascq = sense[3];
    } else {
        key = sense[2] & 0x0F; asc = sense[12]; ascq = sense[13];
    }
    if (key != 0x06 || asc != 0x29)
        return 0;
    t->ua_retried = 1;
    k->sc.xnu.stats.ua_retries++;
    mps_dprint(&k->sc, MPS_RECOVERY, "target %u lun %u: UNIT ATTENTION 29/%02x after our reset, "
               "re-sending the command\n", (unsigned)tid, (unsigned)lun, ascq);
    ccb->ccb_h.status = CAM_REQ_INPROG;
    csio->scsi_status = 0;
    xpt_action(ccb);
    return 1;
}

/*
 * ccb_h.cbfcnp of a SCSI IO: mps_mtx held, so only queue it.  There is no
 * periph: the thin CAM's xpt_done has already released the ccb's
 * CAM_DEV_QFRZN (cam_release_devq's job in FreeBSD), so a retry the family
 * sends later is ordered behind younger IO; disks allow that among SIMPLE
 * tasks. TMs therefore freeze the whole target (cam_compat.c, D17).
 */
static void mps_kext_io_done(struct cam_periph *periph, union ccb *ccb)
{
    (void)periph;
    struct mps_kext_task *t = (struct mps_kext_task *)ccb->ccb_h.ppriv_ptr0;
    struct mps_kext *k = t->k;

    if (t->state == MPSK_T_SUBMIT) {
        t->state = MPSK_T_SYNC_DONE;            /* mps_kext_submit() answers for it */
        return;
    }
    if (mps_kext_retry_ua(k, t))
        return;
    t->state = MPSK_T_QUEUED;
    t->done_next = nullptr;
    mps_os_lock(k->done_lock);
    if (k->done_tail)
        k->done_tail->done_next = t;
    else
        k->done_head = t;
    k->done_tail = t;
    mps_os_unlock(k->done_lock);
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
        mps_os_free(w, sizeof(*w));
        return;
    }
    w->done = 1;
    wakeup(&k->tm_chan);
}

/*
 * CAM async callback (mps_mtx held): AC_XNU_SCAN, AC_BUS_RESET, AC_SENT_BDR.
 * Removals arrive only as AC_XNU_SCAN (the core's rescan after a topology
 * change, and after a firmware DEVICE_NOT_THERE, D20c): AC_LOST_DEVICE comes
 * from CAM's probe and invalidations, which the thin CAM does not have.
 */
static void mps_kext_async(void *arg, uint32_t code, struct cam_path *path, void *a)
{
    struct mps_kext *k = (struct mps_kext *)arg;
    const target_id_t tid = path ? path->target_id : CAM_TARGET_WILDCARD;
    const uint64_t one = (tid < MPSK_MAX_TARGETS) ? (1ull << tid) : 0;
    uint64_t tids = 0, present = 0;
    uint32_t gens[MPSK_MAX_TARGETS];
    int event;

    switch (code) {
    case AC_XNU_SCAN: {
        union ccb *scan = (union ccb *)a;
        event = MPSK_ASYNC_SCAN;
        tids = (tid == CAM_TARGET_WILDCARD || (scan && scan->ccb_h.func_code == XPT_SCAN_BUS)) ? ~0ull : one;
        break;
    }
    case AC_BUS_RESET:
        event = MPSK_ASYNC_BUS_RESET;
        mps_kext_arm_ua(k, CAM_TARGET_WILDCARD, CAM_LUN_WILDCARD);
        break;
    case AC_SENT_BDR:
        event = MPSK_ASYNC_SENT_BDR;
        tids = one;
        if (tid < MPSK_MAX_TARGETS)
            mps_kext_arm_ua(k, tid, path->lun_id);
        break;
    default:
        return;
    }
    for (u_int i = 0; i < MPSK_MAX_TARGETS; i++) {
        gens[i] = mps_xnu_target_gen(&k->sc, i);
        if ((tids & (1ull << i)) && mps_xnu_target_present(&k->sc, i, NULL))
            present |= 1ull << i;
    }
    if (k->host->async)
        k->host->async(k->host->hba, event, tids, present, gens);
}

extern "C" {

struct mps_kext *mps_kext_create(struct mps_kext_host *host, uint16_t devid)
{
    if (mps_kext_os_init() != 0)
        return nullptr;
    struct mps_kext *k = (struct mps_kext *)mps_os_malloc(sizeof(*k), 1);
    if (k == nullptr)
        return nullptr;
    memset(k, 0, sizeof(*k));
    k->done_lock = mps_os_lock_alloc("mpsk_done");
    if (k->done_lock == nullptr) {
        mps_os_free(k, sizeof(*k));
        return nullptr;
    }
    k->host = host;
    mps_compat_device_init(&k->dev, "mps", (int)host->unit, &k->sc, host, devid);
    k->sc.mps_dev = &k->dev;
    k->sc.mps_btag = 0;
    k->sc.mps_bhandle = (bus_space_handle_t)(uintptr_t)host->regs;
    __atomic_add_fetch(&gKexts, 1, __ATOMIC_RELAXED);
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
    callout_init_mtx(&k->hold_callout, &sc->mps_mtx, 0);
    if (xpt_create_path(&path, NULL, cam_sim_path(sc->sassc->sim), CAM_TARGET_WILDCARD,
                        CAM_LUN_WILDCARD) == CAM_REQ_CMP) {
        if (xpt_register_async(AC_XNU_SCAN | AC_BUS_RESET | AC_SENT_BDR,
                               mps_kext_async, k, path) != CAM_REQ_CMP)
            mps_dprint(sc, MPS_FAULT | MPS_INIT, "cannot register the async callback: no hotplug\n");
        xpt_free_path(path);
    }
    return 0;
}

void mps_kext_start(struct mps_kext *k)
{
    mps_compat_intrhook_run(&k->sc);         /* mps_startup: unmask, port enable */
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
    callout_drain(&k->hold_callout);    /* a hold still running: mps_free drops the freeze */
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
    mps_os_lock_free(k->done_lock);
    mps_os_free(k, sizeof(*k));
    __atomic_sub_fetch(&gKexts, 1, __ATOMIC_RELAXED);
}

uint32_t mps_kext_max_segs(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    if (!k->attached || sc->buffer_dmat == NULL || sc->buffer_dmat->dt_nsegments <= 0)
        return 0;
    return (uint32_t)sc->buffer_dmat->dt_nsegments;     /* max_io_pages + 1 (mps.max_io_pages, D32) */
}

/*
 * XNU: the family's ReportMaximumTaskCount: the normal request frames
 * (num_reqs - num_prireqs, FreeBSD's sizing) less MPSK_INTERNAL_CMDS. FreeBSD
 * gives CAM every normal frame (mps_sas.c: reqs = num_reqs - num_prireqs - 1)
 * and its config and IDENTIFY requests then compete with IO for a command:
 * under a full IO load a hot-add's page 0 read finds none (EBUSY) and the add
 * is dropped, while an IO that misses a frame is only requeued. The 8 cover
 * a config page read or SATA IDENTIFY (one at a time, on the event queue),
 * the port enable (1), REMOVE_DEVICE (one per pending removal) and pending
 * event ACKs (D29). 8 of 2048 frames: no throughput change (D21).
 */
uint32_t mps_kext_max_tasks(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    if (!k->attached || sc->num_reqs - sc->num_prireqs <= 0)
        return 0;
    return (uint32_t)MAX(1, sc->num_reqs - sc->num_prireqs - MPSK_INTERNAL_CMDS);
}

static uint64_t DmaTagBytes(bus_dma_tag_t t)
{
    return t != NULL ? (uint64_t)t->dt_maxsize : 0;
}

int mps_kext_limits(struct mps_kext *k, struct mps_kext_limits *l)
{
    struct mps_softc *sc = &k->sc;

    memset_zero(l, sizeof(*l));
    if (!k->attached)
        return -1;
    mps_lock(sc);
    if (sc->facts == NULL) {
        mps_unlock(sc);
        return -1;
    }
    const MPI2_IOC_FACTS_REPLY *f = sc->facts;
    l->request_credit = le16toh(f->RequestCredit);
    l->high_priority_credit = le16toh(f->HighPriorityCredit);
    l->max_reply_post_depth = le16toh(f->MaxReplyDescriptorPostQueueDepth);
    l->max_chain_depth = f->MaxChainDepth;
    l->max_msix_vectors = f->MaxMSIxVectors;
    l->req_frame_bytes = (uint32_t)le16toh(f->IOCRequestFrameSize) * 4;
    l->reply_frame_bytes = (uint32_t)f->ReplyFrameSize * 4;
    l->max_targets = le16toh(f->MaxTargets);
    l->max_volumes = f->MaxVolumes;
    l->req_frames = (uint32_t)(sc->num_reqs - sc->num_prireqs);
    l->pri_req_frames = (uint32_t)sc->num_prireqs;
    l->reply_frames = (uint32_t)sc->num_replies;
    l->post_queue_depth = (uint32_t)sc->pqdepth;
    l->chain_frames = (uint32_t)sc->num_chains;
    l->os_commands = (uint32_t)MAX(0, sc->num_reqs - sc->num_prireqs - MPSK_INTERNAL_CMDS);
    l->max_segs = sc->buffer_dmat != NULL && sc->buffer_dmat->dt_nsegments > 0 ?
        (uint32_t)sc->buffer_dmat->dt_nsegments : 0;
    l->maxio = (uint64_t)sc->maxio;
    l->dma_bytes = DmaTagBytes(sc->queues_dmat) + DmaTagBytes(sc->reply_dmat) + DmaTagBytes(sc->req_dmat) +
        DmaTagBytes(sc->chain_dmat) + DmaTagBytes(sc->sense_dmat);
    /* + one struct bus_dmamap (mps_compat.c: 6 pointer-sized fields) per command */
    l->heap_bytes = (uint64_t)sc->num_reqs * (sizeof(struct mps_command) + 6 * sizeof(void *)) +
        (uint64_t)sc->num_chains * sizeof(struct mps_chain);
    mps_unlock(sc);
    return 0;
}

void *mps_kext_zalloc(size_t size)
{
    void *p = mps_os_malloc(size, 1);
    if (p != NULL)
        memset_zero(p, size);
    return p;
}

void mps_kext_zfree(void *ptr, size_t size)
{
    if (ptr != NULL)
        mps_os_free(ptr, size);
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

/* the family sizes its tasks after InitializeController (mps_attach): segments for buffer_dmat */
size_t mps_kext_task_size(struct mps_kext *k)
{
    return sizeof(struct mps_kext_task) + (size_t)mps_kext_max_segs(k) * sizeof(bus_dma_segment_t);
}

struct mps_kext_seg *mps_kext_task_segs(void *td)
{
    return (struct mps_kext_seg *)mps_kext_task_seglist((struct mps_kext_task *)td);
}

struct mps_kext_task_glue *mps_kext_task_glue(void *td)
{
    return &((struct mps_kext_task *)td)->glue;
}

/* The XPT_SCSI_IO ccb of mock_ioc_test.cpp Rig::NewIo/Go. */
static void mps_kext_queue_done(struct mps_kext *k, struct mps_kext_task *t)
{
    t->state = MPSK_T_QUEUED;
    t->done_next = nullptr;
    mps_os_lock(k->done_lock);
    if (k->done_tail)
        k->done_tail->done_next = t;
    else
        k->done_head = t;
    k->done_tail = t;
    mps_os_unlock(k->done_lock);
}

int mps_kext_submit(struct mps_kext *k, void *td, void *os_task, uint64_t os_tag, uint32_t tid,
                    uint64_t lun, const uint8_t *cdb, uint8_t cdblen, int dir, int attr, uint32_t nsegs, uint64_t len,
                    uint32_t timeout_ms, uint32_t gen, struct mps_kext_io_result *res)
{
    struct mps_softc *sc = &k->sc;
    struct mps_kext_task *t = (struct mps_kext_task *)td;
    union ccb *ccb = &t->ccb;
    int rc;

    memset_zero(ccb, sizeof(*ccb));
    t->k = k;
    t->ua_retried = 0;
    t->os_task = os_task;
    t->os_tag = os_tag;
    t->done_next = nullptr;
    if (!k->attached || cdblen == 0 || cdblen > IOCDBLEN || nsegs > mps_kext_max_segs(k) ||
        len > 0xFFFFFFFFull) {
        /* refused before the core: delivered like any completion */
        ccb->ccb_h.status = k->attached ? CAM_REQ_INVALID : CAM_DEV_NOT_THERE;
        mps_kext_result(t, res);
        mps_kext_queue_done(k, t);
        return MPSK_SUBMIT_DONE;
    }
    t->path.path_id = cam_sim_path(sc->sassc->sim);
    t->path.target_id = tid;
    t->path.lun_id = lun;
    xpt_setup_ccb(&ccb->ccb_h, &t->path, CAM_PRIORITY_NORMAL);
    ccb->ccb_h.func_code = XPT_SCSI_IO;
    ccb->ccb_h.cbfcnp = mps_kext_io_done;
    ccb->ccb_h.ppriv_ptr0 = t;
    /*
     * The family times the task (SetTimeoutForTask -> HandleTimeout); the
     * core's callout is a backstop, later, should that timer be lost.
     * Timeout 0 is the family's "no timeout": FreeBSD's periphs always set
     * one, macOS clients do not, and SAM sends every START STOP UNIT with
     * 0, so a lost reply would hang the PM thread. Such a task gets the
     * core's callout after 10 min (D15), except FORMAT UNIT and SANITIZE,
     * which legitimately run for hours on a large disk.
     */
    if (timeout_ms != 0)
        ccb->ccb_h.timeout = timeout_ms + MPSK_BACKSTOP_MS;
    else if (cdb[0] == 0x04 /* FORMAT UNIT */ || cdb[0] == 0x48 /* SANITIZE */)
        ccb->ccb_h.timeout = 0;
    else
        ccb->ccb_h.timeout = MPSK_UNTIMED_BACKSTOP_MS;
    ccb->ccb_h.flags = (dir == MPSK_DIR_IN ? CAM_DIR_IN : dir == MPSK_DIR_OUT ? CAM_DIR_OUT : CAM_DIR_NONE) |
                       (nsegs ? CAM_DATA_SG_PADDR : 0);
    ccb->csio.data_ptr = nsegs ? (uint8_t *)mps_kext_task_seglist(t) : NULL;
    ccb->csio.sglist_cnt = (uint16_t)nsegs;
    ccb->csio.dxfer_len = (uint32_t)len;
    ccb->csio.sense_len = SSD_FULL_SIZE;
    ccb->csio.cdb_len = cdblen;
    memcpy(ccb->csio.cdb_io.cdb_bytes, cdb, cdblen);
    /*
     * The task's attribute, as da passes its tag_action (BIO_ORDERED ->
     * ORDERED); mpssas_action_scsiio turns it into the request's queue type.
     * No periodic ORDERED tag (scsi_da.c's da_send_ordered): SATL has no
     * ordered NCQ, and the worst latency measured under random load was
     * 1.22 s against the 30 s timeout (D46).
     */
    ccb->csio.tag_action = attr == MPSK_ATTR_ORDERED ? MSG_ORDERED_Q_TAG :
                           attr == MPSK_ATTR_HEAD    ? MSG_HEAD_OF_Q_TAG :
                           attr == MPSK_ATTR_ACA     ? MSG_ACA_TASK : MSG_SIMPLE_Q_TAG;

    /*
     * Not inline_owner: ProcessParallelTask does not deliver completions
     * (no family re-entry from the submit path, as in the pre-FreeBSD driver); anything another
     * ccb completes here goes to the done thread call.
     */
    mps_lock(sc);
    if (tid < MPSK_MAX_TARGETS && gen != mps_xnu_target_gen(sc, tid)) {
        /*
         * Another disk is behind the id now (review C2, D8): this IO was
         * meant for the old one, whose nub the hotplug thread is about to
         * destroy. It must not reach the new disk.
         */
        mps_unlock(sc);
        ccb->ccb_h.status = CAM_DEV_NOT_THERE;
        mps_kext_result(t, res);
        mps_kext_queue_done(k, t);
        return MPSK_SUBMIT_DONE;
    }
    t->state = MPSK_T_SUBMIT;
    xpt_action(ccb);
    if (t->state == MPSK_T_SYNC_DONE) {
        /* refused by mpssas_action_scsiio: the family hears of it through CompleteParallelTask */
        mps_kext_result(t, res);
        mps_kext_queue_done(k, t);
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
    mps_os_lock(k->done_lock);
    struct mps_kext_task *t = k->done_head;
    if (t) {
        k->done_head = t->done_next;
        if (k->done_head == nullptr)
            k->done_tail = nullptr;
    }
    mps_os_unlock(k->done_lock);
    if (t == nullptr)
        return nullptr;
    mps_kext_result(t, res);
    res->td = t;
    t->state = MPSK_T_IDLE;
    return t->os_task;
}

int mps_kext_timeout(struct mps_kext *k, void *td, void *os_task, uint32_t timeout_ms, uint32_t *rearm_ms)
{
    struct mps_softc *sc = &k->sc;
    struct mps_kext_task *t = (struct mps_kext_task *)td;
    int rc;

    *rearm_ms = 0;
    if (!k->attached)
        return ENOENT;
    mps_lock(sc);
    if (t->os_task != os_task || t->state != MPSK_T_POSTED) {
        mps_unlock(sc);
        return ENOENT;                       /* already completed */
    }
    /*
     * D4: CAM never times a ccb that waits in a frozen queue, and the
     * callout covers only the time the IOC owns the command (qos.sim_data:
     * when mpssas_action_scsiio posted it). Time held behind a recovery,
     * a reinit or a sleep does not count.
     */
    if (timeout_ms != 0) {
        uint64_t owned_ms = 0;
        if (!t->ccb.ccb_h.xnu_held && t->ccb.ccb_h.qos.sim_data != 0)
            owned_ms = (uint64_t)(sbinuptime() - (sbintime_t)t->ccb.ccb_h.qos.sim_data) / SBT_1MS;
        if (t->ccb.ccb_h.xnu_held || t->ccb.ccb_h.qos.sim_data == 0 || owned_ms < timeout_ms) {
            /* never 0: SetTimeoutForTask(task, 0) means the task's own timeout */
            *rearm_ms = owned_ms + 1 < timeout_ms ? timeout_ms - (uint32_t)owned_ms : 1;
            mps_dprint(sc, MPS_RECOVERY | MPS_XINFO, "IO for target %u: family timer after %ju ms with the "
                       "IOC%s, re-armed for %u ms\n", t->ccb.ccb_h.target_id, (uintmax_t)owned_ms,
                       t->ccb.ccb_h.xnu_held ? " (held)" : "", *rearm_ms);
            sc->xnu.stats.timer_rearms++;
            mps_unlock(sc);
            return 0;
        }
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
 * OS task management (D10), the FreeBSD way: CAM sends the SIM only
 * XPT_RESET_DEV (mpssas_action_resetdev: a target reset; FreeBSD has no OS
 * LUN reset) and fakes XPT_ABORT; timeouts escalate abort -> LUN reset ->
 * target reset with one TM per target (targ->tm). So:
 *   - ABORT TASK of a posted task: that task's timeout recovery
 *     (mps_xnu_ccb_timeout, queued behind a recovery already running on the
 *     target); the request returns when the task is back. A held task (it
 *     never reached the IOC) completes CAM_REQ_ABORTED at once.
 *   - LUN RESET, TARGET RESET: XPT_RESET_DEV, held behind a frozen device
 *     queue (a recovery, a reinit) like CAM's devq.
 *   - ABORT TASK SET, CLEAR TASK SET, CLEAR ACA: rejected.
 * IOSCSIParallelFamily's *Request methods are synchronous: wait with mps_mtx
 * dropped. The wait pumps the reply queue every 10 ms (KEEP, D43): FreeBSD's
 * own NO_SLEEP waits (mps_wait_command) poll mps_intr_locked, and our
 * completions are delivered in the work loop gate (HandleInterruptRequest ->
 * DeliverCompletions), so a completion handler that issues a TM
 * synchronously holds the gate the interrupt needs. Bounded by the recovery
 * (TM timeouts, then a reinit) and by mps_kext_stop_waiters().
 */
static int mps_kext_tm_wait(struct mps_kext *k, const volatile int *done, struct mps_kext_task *t, uint64_t seq)
{
    struct mps_softc *sc = &k->sc;

    for (;;) {
        if (done ? *done : (t->state != MPSK_T_POSTED || t->ccb.ccb_h.xnu_seq != seq))
            return 1;
        if (k->stopping)
            return 0;
        (void)msleep(&k->tm_chan, &sc->mps_mtx, 0, "mpstm", 10);
        if ((sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE | MPS_FLAGS_XNU_SUSPENDED)) == 0)
            mps_intr_locked(sc);
    }
}

int mps_kext_tm(struct mps_kext *k, uint8_t type, uint32_t tid, uint64_t lun, void *abort_td,
                void *abort_task, uint64_t abort_tag, char *why, size_t whylen)
{
    struct mps_softc *sc = &k->sc;
    struct mps_kext_tmreq *w = nullptr;
    struct mps_kext_task *t = (struct mps_kext_task *)abort_td;
    int result = MPSK_TM_FAILED;

    (void)lun;
    switch (type) {
    case MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK:
    case MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET:
    case MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET:
        break;
    default:
        snprintf(why, whylen, "not supported (FreeBSD sends only aborts and target resets)");
        return MPSK_TM_REJECTED;
    }
    if (type != MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK) {
        w = (struct mps_kext_tmreq *)mps_os_malloc(sizeof(*w), 1);
        if (w == nullptr) {
            snprintf(why, whylen, "no memory");
            return MPSK_TM_FAILED;
        }
        memset_zero(w, sizeof(*w));
        w->k = k;
    }

    mps_lock(sc);
    if (!k->attached || k->stopping || (sc->mps_flags & (MPS_FLAGS_XNU_GONE | MPS_FLAGS_SHUTDOWN))) {
        mps_unlock(sc);
        if (w)
            mps_os_free(w, sizeof(*w));
        snprintf(why, whylen, "controller stopping");
        return MPSK_TM_FAILED;
    }
    k->tm_waiters++;
    if (type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK) {
        if (t == nullptr || t->os_task != abort_task || t->os_tag != abort_tag || t->state != MPSK_T_POSTED) {
            snprintf(why, whylen, "nothing to abort (task already completed)");
            result = MPSK_TM_COMPLETE;
        } else if (cam_xnu_cancel_held(sc->sassc->sim, &t->ccb, CAM_REQ_ABORTED)) {
            snprintf(why, whylen, "held, never sent: completed aborted");
            result = MPSK_TM_COMPLETE;
        } else {
            const uint64_t seq = t->ccb.ccb_h.xnu_seq;
            (void)mps_xnu_ccb_timeout(sc, &t->ccb);
            if (mps_kext_tm_wait(k, nullptr, t, seq)) {
                snprintf(why, whylen, "returned by the target's recovery");
                result = MPSK_TM_COMPLETE;
            } else {
                snprintf(why, whylen, "controller stopping");
            }
        }
    } else if (tid >= sc->sassc->maxtargets || sc->sassc->targets[tid].handle == 0) {
        snprintf(why, whylen, "no device");
    } else {
        w->path.path_id = cam_sim_path(sc->sassc->sim);
        w->path.target_id = tid;
        w->path.lun_id = 0;
        xpt_setup_ccb(&w->ccb.ccb_h, &w->path, CAM_PRIORITY_NORMAL);
        w->ccb.ccb_h.func_code = XPT_RESET_DEV;
        w->ccb.ccb_h.cbfcnp = mps_kext_tm_done;
        w->ccb.ccb_h.ppriv_ptr0 = w;
        xpt_action(&w->ccb);
        if (mps_kext_tm_wait(k, &w->done, nullptr, 0)) {
            const uint32_t st = w->ccb.ccb_h.status & CAM_STATUS_MASK;
            if (st == CAM_REQ_CMP)
                result = MPSK_TM_COMPLETE;
            snprintf(why, whylen, "target reset (XPT_RESET_DEV), CAM status 0x%x", st);
        } else {
            w->orphan = 1;                   /* mps_free() completes it; mps_kext_tm_done frees it */
            w = nullptr;
            snprintf(why, whylen, "controller stopping");
        }
    }
    if (--k->tm_waiters == 0)
        wakeup(&k->tm_waiters);
    mps_unlock(sc);
    if (w)
        mps_os_free(w, sizeof(*w));
    return result;
}

/*
 * Debug triggers for hardware tests (sas2308ctl reinit | hold MS | lose N):
 * a controller reinit (diag reset), new IO held for MS (SIM queue frozen,
 * as a long recovery would), the next N SCSI IOs lost.
 */
static void mps_kext_hold_end(void *arg)
{
    struct mps_kext *k = (struct mps_kext *)arg;

    if (!k->hold_active)
        return;
    k->hold_active = 0;
    mps_dprint(&k->sc, MPS_RECOVERY | MPS_INFO, "debug: IO hold ended\n");
    xpt_release_simq(k->sc.sassc->sim, 1);
}

int mps_kext_debug(struct mps_kext *k, int what, uint32_t arg)
{
    struct mps_softc *sc = &k->sc;
    int error = 0;

    if (!k->attached)
        return ENXIO;
    mps_lock(sc);
    switch (what) {
    case MPSK_DEBUG_REINIT:
        mps_xnu_request_reinit(sc, "requested from userland (debug)");
        break;
    case MPSK_DEBUG_HOLD:
        if (k->hold_active || arg == 0 || arg > 600000) {
            error = EINVAL;
            break;
        }
        k->hold_active = 1;
        xpt_freeze_simq(sc->sassc->sim, 1);
        callout_reset(&k->hold_callout, (int)((uint64_t)arg * hz / 1000), mps_kext_hold_end, k);
        mps_dprint(sc, MPS_RECOVERY | MPS_INFO, "debug: new IO held for %u ms\n", arg);
        break;
    case MPSK_DEBUG_LOSE:
        sc->xnu.debug_lose = arg;
        mps_dprint(sc, MPS_RECOVERY | MPS_INFO, "debug: the next %u SCSI IOs will be lost\n", arg);
        break;
    default:
        error = EINVAL;
    }
    mps_unlock(sc);
    return error;
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
    /* D11: an all-ones read is confirmed from a callout (~100 ms); wait for the verdict */
    for (int i = 0; sc->xnu.gone_tries != 0 && i < 50; i++)
        (void)msleep(&sc->xnu.gone_tries, &sc->mps_mtx, 0, "mpsgone", 10);
    present = (sc->mps_flags & MPS_FLAGS_XNU_GONE) == 0;
    mps_unlock(sc);
    return present;
}

void mps_kext_disable_now(struct mps_kext *k)
{
    struct mps_softc *sc = &k->sc;
    int start;

    if (!k->attached)
        return;
    mps_lock(sc);
    if (sc->mps_flags & MPS_FLAGS_XNU_GONE) {
        /* every wait of mps_reinit checks GONE: it gives up at the next one */
        start = ticks;
        while (sc->xnu.reinit_running && (u_int)(ticks - start) < 10 * hz)
            msleep(&sc->xnu.reinit_running, &sc->mps_mtx, 0, "mpsgone", hz / 100);
        if (sc->xnu.reinit_running)
            mps_dprint(sc, MPS_INIT | MPS_FAULT, "termination: the reinit did not stop in 10 s\n");
        else
            mps_xnu_disable(sc, ENXIO);
    }
    mps_unlock(sc);
}

int mps_kext_suspend(struct mps_kext *k, uint32_t drain_ms, int shutdown)
{
    struct mps_softc *sc = &k->sc;
    int error;

    if (!k->attached)
        return ENXIO;
    mps_lock(sc);
    error = mps_xnu_suspend(sc, drain_ms, shutdown);
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

    memset_zero(info, sizeof(*info));
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

    memset_zero(info, sizeof(*info));
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

int mps_kext_target_drain(struct mps_kext *k, uint32_t tid, uint32_t reinit_ms)
{
    struct mps_softc *sc = &k->sc;
    uint32_t waited;
    int left = 0;

    if (!k->attached)
        return 0;
    mps_lock(sc);
    if (sc->sassc != NULL && sc->sassc->sim != NULL && tid < sc->sassc->maxtargets) {
        (void)cam_xnu_flush_held_target(sc->sassc->sim, tid, CAM_DEV_NOT_THERE);
        /*
         * D14: as long as it takes (CAM frees a periph when its last ccb
         * returns). The IOC owns posted IO and may still DMA into its
         * buffers, so it is never completed here: the removal's target
         * reset returns it, or its TM timeout's reinit does. Should
         * neither have happened after reinit_ms, ask for the reinit (the
         * diag reset ends the IOC's ownership, mpssas_complete_all_commands
         * completes it) and keep waiting. A disabled controller has
         * completed everything; a stop leaves the rest to mps_free.
         */
        for (waited = 0; sc->sassc->targets[tid].outstanding > 0 && !sc->xnu.disabled && !k->stopping;
             waited += 10) {
            if (waited == reinit_ms) {
                mps_dprint(sc, MPS_RECOVERY | MPS_ERROR, "target %u: %u IOs still outstanding %u ms after "
                           "its removal: controller reinit\n", tid, sc->sassc->targets[tid].outstanding,
                           reinit_ms);
                mps_xnu_request_reinit(sc, "removed target's IO not returned");
            }
            (void)msleep(&k->drain_chan, &sc->mps_mtx, 0, "mpsdrain", 10);
            /*
             * Pump the reply queue (FreeBSD's NO_SLEEP waits poll it too);
             * completions still go to the done queue and are delivered
             * from the thread call (D43).
             */
            if ((sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE | MPS_FLAGS_XNU_SUSPENDED)) == 0)
                mps_intr_locked(sc);
        }
        left = (int)sc->sassc->targets[tid].outstanding;
    }
    mps_unlock(sc);
    return left;
}

void *mps_kext_softc(struct mps_kext *k)
{
    return &k->sc;
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

    memset_zero(s, sizeof(*s));
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
    memset_zero(&sc->xnu.stats, sizeof(sc->xnu.stats));
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
    struct mps_softc *tmp = (struct mps_softc *)mps_os_malloc(sizeof(*tmp), 1);
    if (tmp == nullptr)
        return;
    memset(tmp, 0, sizeof(*tmp));
    tmp->mps_debug = *mask;
    strlcpy(buf, s, sizeof(buf));
    mps_parse_debug(tmp, buf);
    *mask = (uint32_t)tmp->mps_debug & MPS_DEBUG_ALL;
    mps_os_free(tmp, sizeof(*tmp));
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

extern "C" {

void *mps_kext_lock_alloc(const char *name) { return mps_os_lock_alloc(name); }
void mps_kext_lock_free(void *lock) { mps_os_lock_free(lock); }
void mps_kext_lock(void *lock) { mps_os_lock(lock); }
void mps_kext_unlock(void *lock) { mps_os_unlock(lock); }
void mps_kext_sleep(void *lock, void *chan) { (void)mps_os_sleep(lock, chan, 0); }
void mps_kext_wakeup(void *chan) { mps_os_wakeup(chan); }

} /* extern "C" */
