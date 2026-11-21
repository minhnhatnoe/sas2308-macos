/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * cam_compat.c - the thin CAM behind cam_compat.h (see the semantics there).
 * All entry points except the allocation helpers expect the SIM's mutex
 * (mps_mtx) to be held.
 */
#include "cam_compat.h"
#include "../mps_xnu.h"	/* mps_xnu_cdb_str: CAM's scsi_op_desc */

#define CAM_XNU_MAXBUS	16

static struct cam_sim *cam_xnu_buses[CAM_XNU_MAXBUS];

/* ---- devq / sim ------------------------------------------------------------ */

struct cam_devq *
cam_simq_alloc(uint32_t max_sim_transactions)
{
	struct cam_devq *devq;

	devq = mps_compat_malloc(sizeof(*devq), NULL, M_NOWAIT | M_ZERO);
	if (devq != NULL)
		devq->max_openings = (int)max_sim_transactions;
	return (devq);
}

void
cam_simq_free(struct cam_devq *devq)
{
	mps_compat_free(devq, NULL);
}

struct cam_sim *
cam_sim_alloc(sim_action_func sim_action, sim_poll_func sim_poll,
    const char *sim_name, void *softc, uint32_t unit, struct mtx *mtx,
    int max_dev_transactions, int max_tagged_dev_transactions,
    struct cam_devq *queue)
{
	struct cam_sim *sim;

	sim = mps_compat_malloc(sizeof(*sim), NULL, M_NOWAIT | M_ZERO);
	if (sim == NULL)
		return (NULL);
	sim->sim_action = sim_action;
	sim->sim_poll = sim_poll;
	sim->sim_name = sim_name;
	sim->softc = softc;
	sim->mtx = mtx;
	sim->path_id = CAM_XPT_PATH_ID;
	sim->unit_number = unit;
	sim->max_dev_openings = max_dev_transactions;
	sim->max_tagged_dev_openings = max_tagged_dev_transactions;
	sim->devq = queue;
	TAILQ_INIT(&sim->held);
	TAILQ_INIT(&sim->frozen);
	TAILQ_INIT(&sim->async);
	return (sim);
}

void
cam_sim_free(struct cam_sim *sim, int free_devq)
{
	struct cam_xnu_frozen *f;
	struct cam_xnu_async *a;

	if (!TAILQ_EMPTY(&sim->held))
		cam_xnu_flush_held(sim, CAM_DEV_NOT_THERE);
	while ((f = TAILQ_FIRST(&sim->frozen)) != NULL) {
		TAILQ_REMOVE(&sim->frozen, f, link);
		mps_compat_free(f, NULL);
	}
	while ((a = TAILQ_FIRST(&sim->async)) != NULL) {
		TAILQ_REMOVE(&sim->async, a, link);
		mps_compat_free(a, NULL);
	}
	if (free_devq && sim->devq != NULL)
		cam_simq_free(sim->devq);
	mps_compat_free(sim, NULL);
}

int
xpt_bus_register(struct cam_sim *sim, device_t parent, uint32_t bus)
{
	struct cam_sim *expect;
	int i;

	(void)parent;
	sim->bus_id = bus;
	for (i = 0; i < CAM_XNU_MAXBUS; i++) {
		expect = NULL;
		if (__atomic_compare_exchange_n(&cam_xnu_buses[i], &expect, sim,
		    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			sim->path_id = (path_id_t)i;
			return (0);
		}
	}
	return (ENOMEM);
}

int
xpt_bus_deregister(path_id_t path_id)
{
	if (path_id >= CAM_XNU_MAXBUS)
		return (EINVAL);
	__atomic_store_n(&cam_xnu_buses[path_id], NULL, __ATOMIC_RELEASE);
	return (0);
}

struct cam_sim *
cam_xnu_sim_for_path(path_id_t path_id)
{
	if (path_id >= CAM_XNU_MAXBUS)
		return (NULL);
	return (__atomic_load_n(&cam_xnu_buses[path_id], __ATOMIC_ACQUIRE));
}

/* ---- paths and ccbs ----------------------------------------------------------- */

cam_status
xpt_create_path(struct cam_path **new_path_ptr, struct cam_periph *perph,
    path_id_t path_id, target_id_t target_id, lun_id_t lun_id)
{
	struct cam_path *path;

	(void)perph;
	path = mps_compat_malloc(sizeof(*path), NULL, M_NOWAIT);
	if (path == NULL)
		return (CAM_RESRC_UNAVAIL);
	path->path_id = path_id;
	path->target_id = target_id;
	path->lun_id = lun_id;
	*new_path_ptr = path;
	return (CAM_REQ_CMP);
}

void
xpt_free_path(struct cam_path *path)
{
	mps_compat_free(path, NULL);
}

void
xpt_setup_ccb(struct ccb_hdr *ccb_h, struct cam_path *path, uint32_t priority)
{
	ccb_h->path = path;
	ccb_h->path_id = path->path_id;
	ccb_h->target_id = path->target_id;
	ccb_h->target_lun = path->lun_id;
	ccb_h->pinfo_priority = priority;
}

union ccb *
xpt_alloc_ccb(void)
{
	return (mps_compat_malloc(sizeof(union ccb), NULL, M_WAITOK | M_ZERO));
}

union ccb *
xpt_alloc_ccb_nowait(void)
{
	return (mps_compat_malloc(sizeof(union ccb), NULL, M_NOWAIT | M_ZERO));
}

void
xpt_free_ccb(union ccb *ccb)
{
	mps_compat_free(ccb, NULL);
}

/* ---- queue freezing and the held queue ---------------------------------------- */

static struct cam_sim *
cam_xnu_sim(union ccb *ccb)
{
	struct cam_sim *sim;
	path_id_t id;

	id = (ccb->ccb_h.path != NULL) ? ccb->ccb_h.path->path_id :
	    ccb->ccb_h.path_id;
	sim = cam_xnu_sim_for_path(id);
	KASSERT(sim != NULL, ("cam: no SIM for path id %u", id));
	return (sim);
}

static struct cam_xnu_frozen *
cam_xnu_find_frozen(struct cam_sim *sim, target_id_t target, lun_id_t lun)
{
	struct cam_xnu_frozen *f;

	TAILQ_FOREACH(f, &sim->frozen, link) {
		if (f->target_id == target && f->lun_id == lun)
			return (f);
	}
	return (NULL);
}

static int
cam_xnu_blocked(struct cam_sim *sim, union ccb *ccb)
{
	struct cam_xnu_frozen *f;

	if (sim->freeze > 0)
		return (1);
	TAILQ_FOREACH(f, &sim->frozen, link) {
		if (f->target_id != ccb->ccb_h.target_id &&
		    f->target_id != CAM_TARGET_WILDCARD)
			continue;
		if (f->lun_id == ccb->ccb_h.target_lun ||
		    f->lun_id == CAM_LUN_WILDCARD)
			return (1);
	}
	return (0);
}

/* Insert by arrival number: a requeued ccb goes back to its old place. */
static void
cam_xnu_hold(struct cam_sim *sim, union ccb *ccb)
{
	struct ccb_hdr *h;

	ccb->ccb_h.status = CAM_REQ_INPROG;
	ccb->ccb_h.xnu_held = 1;
	if (!ccb->ccb_h.xnu_was_held) {
		ccb->ccb_h.xnu_was_held = 1;
		sim->held_total++;
	}
	TAILQ_FOREACH_REVERSE(h, &sim->held, cam_xnu_ccbq, sim_links) {
		if (h->xnu_seq < ccb->ccb_h.xnu_seq)
			break;
	}
	if (h != NULL)
		TAILQ_INSERT_AFTER(&sim->held, h, &ccb->ccb_h, sim_links);
	else
		TAILQ_INSERT_HEAD(&sim->held, &ccb->ccb_h, sim_links);
}

/* Hand every held ccb that is no longer blocked to the SIM, in order. */
static void
cam_xnu_run_held(struct cam_sim *sim)
{
	struct cam_xnu_ccbq batch;
	struct ccb_hdr *h;
	union ccb *ccb;

	if (sim->running) {
		sim->rerun = 1;
		return;
	}
	sim->running = 1;
	do {
		sim->rerun = 0;
		TAILQ_INIT(&batch);
		TAILQ_CONCAT(&batch, &sim->held, sim_links);
		while ((h = TAILQ_FIRST(&batch)) != NULL) {
			TAILQ_REMOVE(&batch, h, sim_links);
			ccb = (union ccb *)h;
			ccb->ccb_h.xnu_held = 0;
			if (cam_xnu_blocked(sim, ccb)) {
				cam_xnu_hold(sim, ccb);
				continue;
			}
			ccb->ccb_h.status = CAM_REQ_INPROG;
			sim->sim_action(sim, ccb);
		}
	} while (sim->rerun);
	sim->running = 0;
}

void
xpt_action(union ccb *ccb)
{
	struct cam_sim *sim;

	sim = cam_xnu_sim(ccb);
	if (sim == NULL) {		/* bus gone (detached): fail, no SIM */
		ccb->ccb_h.status = CAM_PATH_INVALID;
		if (ccb->ccb_h.cbfcnp != NULL)
			ccb->ccb_h.cbfcnp(NULL, ccb);
		return;
	}
	mtx_assert(sim->mtx, MA_OWNED);
	if (ccb->ccb_h.xnu_seq == 0) {
		ccb->ccb_h.xnu_seq = ++sim->seq;
		ccb->ccb_h.xnu_start_us = mps_os_uptime_us();
	}
	/*
	 * Device-queued ccbs (cam_xpt.c xpt_action_default: SCSI IO, reset
	 * device) are held while frozen; the rest goes through.
	 */
	if ((ccb->ccb_h.func_code & XPT_FC_DEV_QUEUED) == XPT_FC_DEV_QUEUED &&
	    (cam_xnu_blocked(sim, ccb) || !TAILQ_EMPTY(&sim->held))) {
		cam_xnu_hold(sim, ccb);
		if (!cam_xnu_blocked(sim, ccb))
			cam_xnu_run_held(sim);
		return;
	}
	ccb->ccb_h.status = CAM_REQ_INPROG;
	sim->sim_action(sim, ccb);
}

uint32_t
xpt_freeze_simq(struct cam_sim *sim, u_int count)
{
	mtx_assert(sim->mtx, MA_OWNED);
	sim->freeze += (int)count;
	return ((uint32_t)sim->freeze);
}

void
xpt_release_simq(struct cam_sim *sim, int run_queue)
{
	mtx_assert(sim->mtx, MA_OWNED);
	if (sim->freeze <= 0) {
		mps_compat_log(NULL, 0, "cam: %s%u: SIM queue released too often",
		    sim->sim_name, sim->unit_number);
		return;
	}
	if (--sim->freeze == 0 && run_queue)
		cam_xnu_run_held(sim);
}

uint32_t
xpt_freeze_devq(struct cam_path *path, u_int count)
{
	struct cam_sim *sim;
	struct cam_xnu_frozen *f;

	sim = cam_xnu_sim_for_path(path->path_id);
	KASSERT(sim != NULL, ("xpt_freeze_devq: no SIM"));
	if (sim == NULL)
		return (0);
	mtx_assert(sim->mtx, MA_OWNED);
	f = cam_xnu_find_frozen(sim, path->target_id, path->lun_id);
	if (f == NULL) {
		f = mps_compat_malloc(sizeof(*f), NULL, M_NOWAIT | M_ZERO);
		if (f == NULL) {
			/* Hold everything rather than lose the freeze. */
			sim->freeze += (int)count;
			return (count);
		}
		f->target_id = path->target_id;
		f->lun_id = path->lun_id;
		TAILQ_INSERT_TAIL(&sim->frozen, f, link);
	}
	f->count += (int)count;
	return ((uint32_t)f->count);
}

static void
cam_xnu_release_dev(struct cam_sim *sim, target_id_t target, lun_id_t lun,
    u_int count)
{
	struct cam_xnu_frozen *f;

	f = cam_xnu_find_frozen(sim, target, lun);
	if (f == NULL) {
		mps_compat_log(NULL, 0, "cam: %s%u: device queue %u:%ju not "
		    "frozen", sim->sim_name, sim->unit_number, target,
		    (uintmax_t)lun);
		return;
	}
	f->count -= ((int)count < f->count) ? (int)count : f->count;
	if (f->count == 0) {
		TAILQ_REMOVE(&sim->frozen, f, link);
		mps_compat_free(f, NULL);
	}
}

void
xpt_release_devq(struct cam_path *path, u_int count, int run_queue)
{
	struct cam_sim *sim;

	sim = cam_xnu_sim_for_path(path->path_id);
	KASSERT(sim != NULL, ("xpt_release_devq: no SIM"));
	if (sim == NULL)
		return;
	mtx_assert(sim->mtx, MA_OWNED);
	cam_xnu_release_dev(sim, path->target_id, path->lun_id, count);
	if (run_queue)
		cam_xnu_run_held(sim);
}

void
xpt_done(union ccb *ccb)
{
	struct cam_sim *sim;
	int run = 0;

	sim = cam_xnu_sim(ccb);
	if (sim == NULL) {		/* KASSERTed: complete it anyway */
		if (ccb->ccb_h.cbfcnp != NULL)
			ccb->ccb_h.cbfcnp(NULL, ccb);
		return;
	}
	mtx_assert(sim->mtx, MA_OWNED);
	if (ccb->ccb_h.status & CAM_DEV_QFRZN) {
		/* The periph's cam_release_devq() in FreeBSD. */
		ccb->ccb_h.status &= ~CAM_DEV_QFRZN;
		cam_xnu_release_dev(sim, ccb->ccb_h.path != NULL ?
		    ccb->ccb_h.path->target_id : ccb->ccb_h.target_id,
		    ccb->ccb_h.path != NULL ? ccb->ccb_h.path->lun_id :
		    ccb->ccb_h.target_lun, 1);
		run = 1;
	}
	if (ccb->ccb_h.status & CAM_RELEASE_SIMQ) {
		ccb->ccb_h.status &= ~CAM_RELEASE_SIMQ;
		if (sim->freeze > 0)
			sim->freeze--;
		run = 1;
	}
	if ((ccb->ccb_h.status & CAM_STATUS_MASK) == CAM_REQUEUE_REQ)
		cam_xnu_hold(sim, ccb);
	else if (ccb->ccb_h.cbfcnp != NULL)
		ccb->ccb_h.cbfcnp(NULL, ccb);
	if (run)
		cam_xnu_run_held(sim);
}

int
cam_xnu_held_count(struct cam_sim *sim)
{
	struct ccb_hdr *h;
	int n = 0;

	TAILQ_FOREACH(h, &sim->held, sim_links)
		n++;
	return (n);
}

void
cam_xnu_flush_held(struct cam_sim *sim, uint32_t status)
{
	struct ccb_hdr *h;
	union ccb *ccb;

	while ((h = TAILQ_FIRST(&sim->held)) != NULL) {
		TAILQ_REMOVE(&sim->held, h, sim_links);
		ccb = (union ccb *)h;
		ccb->ccb_h.xnu_held = 0;
		ccb->ccb_h.status = status;
		if (ccb->ccb_h.cbfcnp != NULL)
			ccb->ccb_h.cbfcnp(NULL, ccb);
	}
}

/* XNU: fail the held ccbs of one target (its nub is about to go) */
int
cam_xnu_flush_held_target(struct cam_sim *sim, target_id_t tid, uint32_t status)
{
	struct ccb_hdr *h, *next;
	union ccb *ccb;
	int n = 0;

	mtx_assert(sim->mtx, MA_OWNED);
	for (h = TAILQ_FIRST(&sim->held); h != NULL; h = next) {
		next = TAILQ_NEXT(h, sim_links);
		if (h->target_id != tid)
			continue;
		TAILQ_REMOVE(&sim->held, h, sim_links);
		ccb = (union ccb *)h;
		ccb->ccb_h.xnu_held = 0;
		ccb->ccb_h.status = status;
		if (ccb->ccb_h.cbfcnp != NULL)
			ccb->ccb_h.cbfcnp(NULL, ccb);
		n++;
	}
	return (n);
}

int
cam_xnu_cancel_held(struct cam_sim *sim, union ccb *ccb, uint32_t status)
{
	mtx_assert(sim->mtx, MA_OWNED);
	if (!ccb->ccb_h.xnu_held)
		return (0);
	TAILQ_REMOVE(&sim->held, &ccb->ccb_h, sim_links);
	ccb->ccb_h.xnu_held = 0;
	ccb->ccb_h.status = status;
	if (ccb->ccb_h.cbfcnp != NULL)
		ccb->ccb_h.cbfcnp(NULL, ccb);
	return (1);
}

/* ---- async callbacks, rescans ------------------------------------------------- */

static void
cam_xnu_async_dispatch(struct cam_sim *sim, uint32_t code,
    struct cam_path *path, void *arg)
{
	struct cam_xnu_async *a, *next;

	TAILQ_FOREACH_SAFE(a, &sim->async, link, next) {
		if (a->events & code)
			a->callback(a->arg, code, path, arg);
	}
}

void
xpt_async(uint32_t async_code, struct cam_path *path, void *async_arg)
{
	struct cam_sim *sim;

	sim = cam_xnu_sim_for_path(path->path_id);
	if (sim == NULL)
		return;
	cam_xnu_async_dispatch(sim, async_code, path, async_arg);
}

void
xpt_rescan(union ccb *ccb)
{
	struct cam_sim *sim;

	sim = cam_xnu_sim(ccb);
	if (sim != NULL) {
		mtx_assert(sim->mtx, MA_OWNED);
		cam_xnu_async_dispatch(sim, AC_XNU_SCAN, ccb->ccb_h.path, ccb);
	}
	xpt_free_path(ccb->ccb_h.path);
	xpt_free_ccb(ccb);
}

cam_status
xpt_register_async(int event, void (*cbfunc)(void *, uint32_t,
    struct cam_path *, void *), void *cbarg, struct cam_path *path)
{
	struct cam_sim *sim;
	struct cam_xnu_async *a;
	int locked;

	sim = cam_xnu_sim_for_path(path->path_id);
	if (sim == NULL)
		return (CAM_PATH_INVALID);
	locked = !mtx_owned(sim->mtx);
	if (locked)
		mtx_lock(sim->mtx);
	TAILQ_FOREACH(a, &sim->async, link) {
		if (a->callback == cbfunc && a->arg == cbarg)
			break;
	}
	if (event == 0) {
		if (a != NULL) {
			TAILQ_REMOVE(&sim->async, a, link);
			mps_compat_free(a, NULL);
		}
	} else if (a != NULL) {
		a->events = (uint32_t)event;
	} else {
		a = mps_compat_malloc(sizeof(*a), NULL, M_NOWAIT | M_ZERO);
		if (a == NULL) {
			if (locked)
				mtx_unlock(sim->mtx);
			return (CAM_RESRC_UNAVAIL);
		}
		a->events = (uint32_t)event;
		a->callback = cbfunc;
		a->arg = cbarg;
		TAILQ_INSERT_TAIL(&sim->async, a, link);
	}
	if (locked)
		mtx_unlock(sim->mtx);
	return (CAM_REQ_CMP);
}

/* ---- bus_dmamap_load_ccb ------------------------------------------------------ */

/*
 * XNU: the platform hands the data as CAM_DATA_SG_PADDR, a list of bus
 * addresses (IODMACommand segments).  Anything else, a list that does not
 * add up to dxfer_len (EINVAL), more segments than the tag allows or a
 * segment longer than its maxsegsz (EFBIG) is refused, and the callback is
 * not called.
 */
int
bus_dmamap_load_ccb(bus_dma_tag_t dmat, bus_dmamap_t map, union ccb *ccb,
    bus_dmamap_callback_t *callback, void *callback_arg, int flags)
{
	struct ccb_scsiio *csio = &ccb->csio;
	bus_dma_segment_t *segs;
	uint64_t total = 0;
	int i;

	(void)map;
	(void)flags;
	if ((ccb->ccb_h.flags & CAM_DATA_MASK) != CAM_DATA_SG_PADDR ||
	    (ccb->ccb_h.flags & CAM_DIR_MASK) == CAM_DIR_NONE ||
	    csio->data_ptr == NULL || csio->sglist_cnt == 0)
		return (EINVAL);
	if (dmat->dt_nsegments > 0 && csio->sglist_cnt > dmat->dt_nsegments)
		return (EFBIG);
	/* the list must describe exactly dxfer_len bytes, in legal pieces */
	segs = (bus_dma_segment_t *)(void *)csio->data_ptr;
	for (i = 0; i < csio->sglist_cnt; i++) {
		if (segs[i].ds_len == 0)
			return (EINVAL);
		if (segs[i].ds_len > dmat->dt_maxsegsz)
			return (EFBIG);
		total += segs[i].ds_len;
	}
	if (total != csio->dxfer_len || total > dmat->dt_maxsize)
		return (EINVAL);
	callback(callback_arg, (bus_dma_segment_t *)(void *)csio->data_ptr,
	    csio->sglist_cnt, 0);
	return (0);
}

/* ---- path / command strings ---------------------------------------------------- */

void
xpt_path_sbuf(struct cam_path *path, struct sbuf *sb)
{
	struct cam_sim *sim;

	sim = cam_xnu_sim_for_path(path->path_id);
	sbuf_printf(sb, "(%s%u:%u:%u:%ju): ", sim ? sim->sim_name : "xpt",
	    sim ? sim->unit_number : 0, sim ? sim->bus_id : 0,
	    path->target_id, (uintmax_t)path->lun_id);
}

void
scsi_command_string(struct ccb_scsiio *csio, struct sbuf *sb)
{
	const uint8_t *cdb;
	int i;

	cdb = (csio->ccb_h.flags & CAM_CDB_POINTER) ? csio->cdb_io.cdb_ptr :
	    csio->cdb_io.cdb_bytes;
	if (cdb == NULL || csio->cdb_len == 0)
		return;
	sbuf_printf(sb, "%s. CDB:", mps_xnu_cdb_str(cdb[0]));
	for (i = 0; i < csio->cdb_len; i++)
		sbuf_printf(sb, " %02x", cdb[i]);
	sbuf_printf(sb, " ");
}
