/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2009 Yahoo! Inc.
 * Copyright (c) 2011-2015 LSI Corp.
 * Copyright (c) 2013-2015 Avago Technologies
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Avago Technologies (LSI) MPT-Fusion Host Adapter FreeBSD
 */

#include "compat/mps_compat.h"
#include "compat/cam_compat.h"
#include "mps_mpi.h"
#include "mpsvar.h"
#include "mps_sas.h"
#include "mps_table.h"
/* Communications core for Avago Technologies (LSI) MPT2 */

/* TODO Move headers to mpsvar */






MALLOC_DEFINE(M_MPSSAS, "MPSSAS", "MPS SAS memory");

static void mpssas_remove_device(struct mps_softc *, struct mps_command *);
static void mpssas_xnu_send_remove(struct mps_softc *, struct mpssas_target *,
    struct mps_command *);
static void mpssas_remove_complete(struct mps_softc *, struct mps_command *);
static void mpssas_action(struct cam_sim *sim, union ccb *ccb);
static void mpssas_poll(struct cam_sim *sim);
static int mpssas_send_abort(struct mps_softc *sc, struct mps_command *tm,
    struct mps_command *cm);
static void mpssas_scsiio_timeout(void *data);
static void mpssas_abort_complete(struct mps_softc *sc, struct mps_command *cm);
static void mpssas_action_scsiio(struct mpssas_softc *, union ccb *);
static void mpssas_scsiio_complete(struct mps_softc *, struct mps_command *);
static void mpssas_action_resetdev(struct mpssas_softc *, union ccb *);
static void mpssas_resetdev_complete(struct mps_softc *, struct mps_command *);
/* XNU: OS task management, queued recoveries */
static void mpssas_action_xnu_tm(struct mpssas_softc *, union ccb *);
static void mpssas_xnu_tm_complete(struct mps_softc *, struct mps_command *);
static void mpssas_xnu_retry_recovery(struct mps_softc *);
static int mpssas_send_portenable(struct mps_softc *sc);
static void mpssas_portenable_complete(struct mps_softc *sc,
    struct mps_command *cm);

struct mpssas_target *
mpssas_find_target_by_handle(struct mpssas_softc *sassc, int start, uint16_t handle)
{
	struct mpssas_target *target;
	int i;

	for (i = start; i < sassc->maxtargets; i++) {
		target = &sassc->targets[i];
		if (target->handle == handle)
			return (target);
	}

	return (NULL);
}

/* we need to freeze the simq during attach and diag reset, to avoid failing
 * commands before device handles have been found by discovery.  Since
 * discovery involves reading config pages and possibly sending commands,
 * discovery actions may continue even after we receive the end of discovery
 * event, so refcount discovery actions instead of assuming we can unfreeze
 * the simq when we get the event.
 */
void
mpssas_startup_increment(struct mpssas_softc *sassc)
{
	MPS_FUNCTRACE(sassc->sc);

	if ((sassc->flags & MPSSAS_IN_STARTUP) != 0) {
		if (sassc->startup_refcount++ == 0) {
			/* just starting, freeze the simq */
			mps_dprint(sassc->sc, MPS_INIT,
			    "%s freezing simq\n", __func__);
			xpt_hold_boot();
			xpt_freeze_simq(sassc->sim, 1);
		}
		mps_dprint(sassc->sc, MPS_INIT, "%s refcount %u\n", __func__,
		    sassc->startup_refcount);
	}
}

void
mpssas_release_simq_reinit(struct mpssas_softc *sassc)
{
	if (sassc->flags & MPSSAS_QUEUE_FROZEN) {
		sassc->flags &= ~MPSSAS_QUEUE_FROZEN;
		xpt_release_simq(sassc->sim, 1);
		mps_dprint(sassc->sc, MPS_INFO, "Unfreezing SIM queue\n");
	}
}

void
mpssas_startup_decrement(struct mpssas_softc *sassc)
{
	MPS_FUNCTRACE(sassc->sc);

	if ((sassc->flags & MPSSAS_IN_STARTUP) != 0) {
		if (--sassc->startup_refcount == 0) {
			/* finished all discovery-related actions, release
			 * the simq and rescan for the latest topology.
			 */
			mps_dprint(sassc->sc, MPS_INIT,
			    "%s releasing simq\n", __func__);
			sassc->flags &= ~MPSSAS_IN_STARTUP;
			xpt_release_simq(sassc->sim, 1);
			xpt_release_boot();
		}
		mps_dprint(sassc->sc, MPS_INIT, "%s refcount %u\n", __func__,
		    sassc->startup_refcount);
	}
}

/*
 * The firmware requires us to stop sending commands when we're doing task
 * management.
 * XXX The logic for serializing the device has been made lazy and moved to
 * mpssas_prepare_for_tm().
 */
struct mps_command *
mpssas_alloc_tm(struct mps_softc *sc)
{
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	struct mps_command *tm;

	tm = mps_alloc_high_priority_command(sc);
	if (tm == NULL)
		return (NULL);

	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	req->Function = MPI2_FUNCTION_SCSI_TASK_MGMT;
	return tm;
}

void
mpssas_free_tm(struct mps_softc *sc, struct mps_command *tm)
{
	if (tm == NULL)
		return;

	/*
	 * For TM's the devq is frozen for the device.  Unfreeze it here and
	 * free the resources used for freezing the devq.  Must clear the
	 * INRESET flag as well or scsi I/O will not work.
	 */
	if (tm->cm_ccb) {
		mps_dprint(sc, MPS_XINFO | MPS_RECOVERY,
		    "Unfreezing devq for target ID %d\n",
		    tm->cm_targ->tid);
		tm->cm_targ->flags &= ~MPSSAS_TARGET_INRESET;
		xpt_release_devq(tm->cm_ccb->ccb_h.path, 1, TRUE);
		xpt_free_path(tm->cm_ccb->ccb_h.path);
		xpt_free_ccb(tm->cm_ccb);
	}

	mps_free_high_priority_command(sc, tm);
	mpssas_xnu_retry_recovery(sc);	/* XNU: a queued recovery may start */
}

void
mpssas_rescan_target(struct mps_softc *sc, struct mpssas_target *targ)
{
	struct mpssas_softc *sassc = sc->sassc;
	path_id_t pathid;
	target_id_t targetid;
	union ccb *ccb;

	MPS_FUNCTRACE(sc);
	pathid = cam_sim_path(sassc->sim);
	if (targ == NULL)
		targetid = CAM_TARGET_WILDCARD;
	else
		targetid = targ - sassc->targets;

	/*
	 * Allocate a CCB and schedule a rescan.
	 */
	ccb = xpt_alloc_ccb_nowait();
	if (ccb == NULL) {
		mps_dprint(sc, MPS_ERROR, "unable to alloc CCB for rescan\n");
		return;
	}

	if (xpt_create_path(&ccb->ccb_h.path, NULL, pathid,
	    targetid, CAM_LUN_WILDCARD) != CAM_REQ_CMP) {
		mps_dprint(sc, MPS_ERROR, "unable to create path for rescan\n");
		xpt_free_ccb(ccb);
		return;
	}

	if (targetid == CAM_TARGET_WILDCARD)
		ccb->ccb_h.func_code = XPT_SCAN_BUS;
	else
		ccb->ccb_h.func_code = XPT_SCAN_TGT;     

	mps_dprint(sc, MPS_TRACE, "%s targetid %u\n", __func__, targetid);
	xpt_rescan(ccb);
}

static void
mpssas_log_command(struct mps_command *cm, u_int level, const char *fmt, ...)
{
	struct sbuf sb;
	va_list ap;
	char str[224];

	if (cm == NULL)
		return;

	/* No need to be in here if debugging isn't enabled */
	if ((cm->cm_sc->mps_debug & level) == 0)
		return;

	sbuf_new(&sb, str, sizeof(str), 0);

	va_start(ap, fmt);

	if (cm->cm_ccb != NULL) {
		xpt_path_sbuf(cm->cm_ccb->csio.ccb_h.path, &sb);
		if (cm->cm_ccb->ccb_h.func_code == XPT_SCSI_IO) {
			scsi_command_string(&cm->cm_ccb->csio, &sb);
			sbuf_printf(&sb, "length %d ",
				    cm->cm_ccb->csio.dxfer_len);
		}
	}
	else {
		sbuf_printf(&sb, "(noperiph:%s%d:%u:%u:%u): ",
		    cam_sim_name(cm->cm_sc->sassc->sim),
		    cam_sim_unit(cm->cm_sc->sassc->sim),
		    cam_sim_bus(cm->cm_sc->sassc->sim),
		    cm->cm_targ ? cm->cm_targ->tid : 0xFFFFFFFF,
		    cm->cm_lun);
	}

	sbuf_printf(&sb, "SMID %u ", cm->cm_desc.Default.SMID);
	sbuf_vprintf(&sb, fmt, ap);
	sbuf_finish(&sb);
	/* XNU: keep the line's class (os_log category) */
	mps_compat_log(cm->cm_sc->mps_dev, level, "%s", sbuf_data(&sb));

	va_end(ap);
}

/*
 * No Need to call "MPI2_SAS_OP_REMOVE_DEVICE" For Volume removal.
 * Otherwise Volume Delete is same as Bare Drive Removal.
 */
/*
 * The MPT2 firmware performs debounce on the link to avoid transient link
 * errors and false removals.  When it does decide that link has been lost
 * and a device need to go away, it expects that the host will perform a
 * target reset and then an op remove.  The reset has the side-effect of
 * aborting any outstanding requests for the device, which is required for
 * the op-remove to succeed.  It's not clear if the host should check for
 * the device coming back alive after the reset.
 */
void
mpssas_prepare_remove(struct mpssas_softc *sassc, uint16_t handle)
{
	struct mps_softc *sc;
	struct mps_command *cm;
	struct mpssas_target *targ = NULL;

	MPS_FUNCTRACE(sassc->sc);

	sc = sassc->sc;

	targ = mpssas_find_target_by_handle(sassc, 0, handle);
	if (targ == NULL) {
		/* FIXME: what is the action? */
		/* We don't know about this device? */
		mps_dprint(sc, MPS_ERROR,
		    "%s : invalid handle 0x%x \n", __func__, handle);
		return;
	}

	targ->flags |= MPSSAS_TARGET_INREMOVAL;

	/*
	 * XNU: the OS learns of the removal now, whether or not a TM is
	 * free; a removal that finds none waits for one (mpssas_free_tm ->
	 * mpssas_xnu_retry_recovery) instead of being dropped: one event
	 * batch can remove more devices than there are TMs.
	 */
	mpssas_rescan_target(sc, targ);

	cm = mpssas_alloc_tm(sc);
	if (cm == NULL) {
		mps_dprint(sc, MPS_ERROR|MPS_RECOVERY,
		    "%s: command alloc failure, removal of target %u queued\n",
		    __func__, targ->tid);
		targ->flags |= MPSSAS_TARGET_XNU_REMOVE_WAIT;
		sc->xnu.stats.removals_queued++;
		return;
	}
	mpssas_xnu_send_remove(sc, targ, cm);
}

/* XNU: the target reset that starts a removal (mpssas_prepare_remove) */
static void
mpssas_xnu_send_remove(struct mps_softc *sc, struct mpssas_target *targ,
    struct mps_command *cm)
{
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	uint16_t handle = targ->handle;

	targ->flags &= ~MPSSAS_TARGET_XNU_REMOVE_WAIT;
	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)cm->cm_req;
	req->DevHandle = htole16(targ->handle);
	req->TaskType = MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET;

	/* SAS Hard Link Reset / SATA Link Reset */
	req->MsgFlags = MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET;

	cm->cm_targ = targ;
	cm->cm_data = NULL;
	cm->cm_complete = mpssas_remove_device;
	cm->cm_complete_data = (void *)(uintptr_t)handle;

	mps_dprint(sc, MPS_INFO, "%s: Sending reset for target ID %d\n",
	    __func__, targ->tid);
	mpssas_prepare_for_tm(sc, cm, targ, CAM_LUN_WILDCARD);

	mps_map_command(sc, cm);
}

static void
mpssas_remove_device(struct mps_softc *sc, struct mps_command *tm)
{
	MPI2_SCSI_TASK_MANAGE_REPLY *reply;
	MPI2_SAS_IOUNIT_CONTROL_REQUEST *req;
	struct mpssas_target *targ;
	uint16_t handle;

	MPS_FUNCTRACE(sc);

	reply = (MPI2_SCSI_TASK_MANAGE_REPLY *)tm->cm_reply;
	handle = (uint16_t)(uintptr_t)tm->cm_complete_data;
	targ = tm->cm_targ;

	/*
	 * Currently there should be no way we can hit this case.  It only
	 * happens when we have a failure to allocate chain frames, and
	 * task management commands don't have S/G lists.
	 */
	if ((tm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		mps_dprint(sc, MPS_ERROR,
		    "%s: cm_flags = %#x for remove of handle %#04x! "
		    "This should not happen!\n", __func__, tm->cm_flags,
		    handle);
	}

	if (reply == NULL) {
		/* XXX retry the remove after the diag reset completes? */
		mps_dprint(sc, MPS_FAULT,
		    "%s NULL reply resetting device 0x%04x\n", __func__,
		    handle);
		mpssas_free_tm(sc, tm);
		return;
	}

	if ((le16toh(reply->IOCStatus) & MPI2_IOCSTATUS_MASK) !=
	    MPI2_IOCSTATUS_SUCCESS) {
		mps_dprint(sc, MPS_ERROR,
		   "IOCStatus = 0x%x while resetting device 0x%x\n",
		   le16toh(reply->IOCStatus), handle);
	}

	mps_dprint(sc, MPS_XINFO, "Reset aborted %u commands\n",
	    le32toh(reply->TerminationCount));
	mps_free_reply(sc, tm->cm_reply_data);
	tm->cm_reply = NULL;	/* Ensures the reply won't get re-freed */

	/* Reuse the existing command */
	req = (MPI2_SAS_IOUNIT_CONTROL_REQUEST *)tm->cm_req;
	memset(req, 0, sizeof(*req));
	req->Function = MPI2_FUNCTION_SAS_IO_UNIT_CONTROL;
	req->Operation = MPI2_SAS_OP_REMOVE_DEVICE;
	req->DevHandle = htole16(handle);
	tm->cm_data = NULL;
	tm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	tm->cm_complete = mpssas_remove_complete;
	tm->cm_complete_data = (void *)(uintptr_t)handle;

	/*
	 * Wait to send the REMOVE_DEVICE until all the commands have cleared.
	 * They should be aborted or time out and we'll kick thus off there
	 * if so.
	 */
	if (TAILQ_FIRST(&targ->commands) == NULL) {
		mps_dprint(sc, MPS_INFO,
		    "No pending commands: starting remove_device target %u handle 0x%04x\n",
		    targ->tid, handle);
		mps_map_command(sc, tm);
		targ->pending_remove_tm = NULL;
	} else {
		targ->pending_remove_tm = tm;
	}

	mps_dprint(sc, MPS_XINFO, "clearing target %u handle 0x%04x\n",
		   targ->tid, handle);
}

static void
mpssas_remove_complete(struct mps_softc *sc, struct mps_command *tm)
{
	MPI2_SAS_IOUNIT_CONTROL_REPLY *reply;
	uint16_t handle;
	struct mpssas_target *targ;
	struct mpssas_lun *lun;

	MPS_FUNCTRACE(sc);

	reply = (MPI2_SAS_IOUNIT_CONTROL_REPLY *)tm->cm_reply;
	handle = (uint16_t)(uintptr_t)tm->cm_complete_data;
	targ = tm->cm_targ;

	/*
	 * At this point, we should have no pending commands for the target.
	 * The remove target has just completed.
	 */
	KASSERT(TAILQ_FIRST(&targ->commands) == NULL,
	    ("%s: no commands should be pending\n", __func__));

	/*
	 * Currently there should be no way we can hit this case.  It only
	 * happens when we have a failure to allocate chain frames, and
	 * task management commands don't have S/G lists.
	 */
	if ((tm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		mps_dprint(sc, MPS_XINFO,
			   "%s: cm_flags = %#x for remove of handle %#04x! "
			   "This should not happen!\n", __func__, tm->cm_flags,
			   handle);
		mpssas_free_tm(sc, tm);
		return;
	}

	if (reply == NULL) {
		/* most likely a chip reset */
		mps_dprint(sc, MPS_FAULT,
		    "%s NULL reply removing device 0x%04x\n", __func__, handle);
		mpssas_free_tm(sc, tm);
		return;
	}

	mps_dprint(sc, MPS_XINFO,
	    "%s on handle 0x%04x, IOCStatus= 0x%x\n", __func__, 
	    handle, le16toh(reply->IOCStatus));

	/*
	 * Don't clear target if remove fails because things will get confusing.
	 * Leave the devname and sasaddr intact so that we know to avoid reusing
	 * this target id if possible, and so we can assign the same target id
	 * to this device if it comes back in the future.
	 */
	if ((le16toh(reply->IOCStatus) & MPI2_IOCSTATUS_MASK) ==
	    MPI2_IOCSTATUS_SUCCESS) {
		targ->handle = 0x0;
		targ->encl_handle = 0x0;
		targ->encl_slot = 0x0;
		targ->exp_dev_handle = 0x0;
		targ->phy_num = 0x0;
		targ->linkrate = 0x0;
		targ->devinfo = 0x0;
		targ->flags = 0x0;
		
		while(!SLIST_EMPTY(&targ->luns)) {
			lun = SLIST_FIRST(&targ->luns);
			SLIST_REMOVE_HEAD(&targ->luns, lun_link);
			free(lun, M_MPT2);
		}
	}

	mpssas_free_tm(sc, tm);
}

static int
mpssas_register_events(struct mps_softc *sc)
{
	u32 events[MPI2_EVENT_NOTIFY_EVENTMASK_WORDS];

	bzero(events, 16);
	setbit(events, MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE);
	setbit(events, MPI2_EVENT_SAS_DISCOVERY);
	setbit(events, MPI2_EVENT_SAS_BROADCAST_PRIMITIVE);
	setbit(events, MPI2_EVENT_SAS_INIT_DEVICE_STATUS_CHANGE);
	setbit(events, MPI2_EVENT_SAS_INIT_TABLE_OVERFLOW);
	setbit(events, MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST);
	setbit(events, MPI2_EVENT_SAS_ENCL_DEVICE_STATUS_CHANGE);
	setbit(events, MPI2_EVENT_IR_CONFIGURATION_CHANGE_LIST);
	setbit(events, MPI2_EVENT_IR_VOLUME);
	setbit(events, MPI2_EVENT_IR_PHYSICAL_DISK);
	setbit(events, MPI2_EVENT_IR_OPERATION_STATUS);
	setbit(events, MPI2_EVENT_LOG_ENTRY_ADDED);

	/* XNU: a failed registration fails the attach */
	return (mps_register_events(sc, events, mpssas_evt_handler, NULL,
	    &sc->sassc->mpssas_eh));
}

int
mps_attach_sas(struct mps_softc *sc)
{
	struct mpssas_softc *sassc;
	int unit, error = 0, reqs;

	MPS_FUNCTRACE(sc);
	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);

	sassc = malloc(sizeof(struct mpssas_softc), M_MPT2, M_WAITOK|M_ZERO);
	if (sassc == NULL)		/* XNU: IOMalloc can fail */
		return (ENOMEM);

	/*
	 * XXX MaxTargets could change during a reinit.  Since we don't
	 * resize the targets[] array during such an event, cache the value
	 * of MaxTargets here so that we don't get into trouble later.  This
	 * should move into the reinit logic.
	 */
	sassc->maxtargets = sc->facts->MaxTargets + sc->facts->MaxVolumes;
	sassc->targets = malloc(sizeof(struct mpssas_target) *
	    sassc->maxtargets, M_MPT2, M_WAITOK|M_ZERO);
	if (sassc->targets == NULL) {	/* XNU: IOMalloc can fail */
		free(sassc, M_MPT2);
		return (ENOMEM);
	}
	sc->sassc = sassc;
	sassc->sc = sc;

	reqs = sc->num_reqs - sc->num_prireqs - 1;
	if ((sassc->devq = cam_simq_alloc(reqs)) == NULL) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate SIMQ\n");
		error = ENOMEM;
		goto out;
	}

	unit = device_get_unit(sc->mps_dev);
	sassc->sim = cam_sim_alloc(mpssas_action, mpssas_poll, "mps", sassc,
	    unit, &sc->mps_mtx, reqs, reqs, sassc->devq);
	if (sassc->sim == NULL) {
		mps_dprint(sc, MPS_INIT|MPS_ERROR, "Cannot allocate SIM\n");
		error = EINVAL;
		goto out;
	}

	TAILQ_INIT(&sassc->ev_queue);

	/* Initialize taskqueue for Event Handling */
	TASK_INIT(&sassc->ev_task, 0, mpssas_firmware_event_work, sc);
	sassc->ev_tq = taskqueue_create("mps_taskq", M_NOWAIT | M_ZERO,
	    taskqueue_thread_enqueue, &sassc->ev_tq);
	/* XNU: both can fail (allocation, thread call) */
	if (sassc->ev_tq == NULL || taskqueue_start_threads(&sassc->ev_tq, 1,
	    PRIBIO, "%s taskq", device_get_nameunit(sc->mps_dev)) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_ERROR, "Cannot start the event "
		    "taskqueue\n");
		error = ENOMEM;
		goto out;
	}

	mps_lock(sc);

	/*
	 * XXX There should be a bus for every port on the adapter, but since
	 * we're just going to fake the topology for now, we'll pretend that
	 * everything is just a target on a single bus.
	 */
	if ((error = xpt_bus_register(sassc->sim, sc->mps_dev, 0)) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_ERROR,
		    "Error %d registering SCSI bus\n", error);
		mps_unlock(sc);
		goto out;
	}

	/*
	 * Assume that discovery events will start right away.
	 *
	 * Hold off boot until discovery is complete.
	 */
	sassc->flags |= MPSSAS_IN_STARTUP | MPSSAS_IN_DISCOVERY;
	sc->sassc->startup_refcount = 0;
	mpssas_startup_increment(sassc);

	mps_unlock(sc);

	/* XNU: no EEDP, so no AC_ADVINFO_CHANGED handler (mpssas_async) */
	error = mpssas_register_events(sc);	/* XNU: checked */
out:
	if (error)
		mps_detach_sas(sc);

	mps_dprint(sc, MPS_INIT, "%s exit error= %d\n", __func__, error);
	return (error);
}

int
mps_detach_sas(struct mps_softc *sc)
{
	struct mpssas_softc *sassc;
	struct mpssas_lun *lun, *lun_tmp;
	struct mpssas_target *targ;
	int i;

	MPS_FUNCTRACE(sc);

	if (sc->sassc == NULL)
		return (0);

	sassc = sc->sassc;
	mps_deregister_events(sc, sassc->mpssas_eh);

	/*
	 * Drain and free the event handling taskqueue with the lock
	 * unheld so that any parallel processing tasks drain properly
	 * without deadlocking.
	 */
	if (sassc->ev_tq != NULL)
		taskqueue_free(sassc->ev_tq);

	/* XNU: no async handler to deregister (no EEDP) */

	/* Make sure CAM doesn't wedge if we had to bail out early. */
	mps_lock(sc);

	while (sassc->startup_refcount != 0)
		mpssas_startup_decrement(sassc);

	if (sassc->flags & MPSSAS_IN_STARTUP)
		xpt_release_simq(sassc->sim, 1);

	if (sassc->sim != NULL) {
		xpt_bus_deregister(cam_sim_path(sassc->sim));
		cam_sim_free(sassc->sim, FALSE);
	}

	mps_unlock(sc);

	if (sassc->devq != NULL)
		cam_simq_free(sassc->devq);

	for(i=0; i< sassc->maxtargets ;i++) {
		targ = &sassc->targets[i];
		SLIST_FOREACH_SAFE(lun, &targ->luns, lun_link, lun_tmp) {
			free(lun, M_MPT2);
		}
	}
	free(sassc->targets, M_MPT2);
	free(sassc, M_MPT2);
	sc->sassc = NULL;

	return (0);
}

void
mpssas_discovery_end(struct mpssas_softc *sassc)
{
	struct mps_softc *sc = sassc->sc;

	MPS_FUNCTRACE(sc);

	/*
	 * After discovery has completed, check the mapping table for any
	 * missing devices and update their missing counts. Only do this once
	 * whenever the driver is initialized so that missing counts aren't
	 * updated unnecessarily. Note that just because discovery has
	 * completed doesn't mean that events have been processed yet. The
	 * check_devices function is a callout timer that checks if ALL devices
	 * are missing. If so, it will wait a little longer for events to
	 * complete and keep resetting itself until some device in the mapping
	 * table is not missing, meaning that event processing has started.
	 */
	/* XNU: no mapping table (mps_mapping_check_devices) */
}

static void
mpssas_action(struct cam_sim *sim, union ccb *ccb)
{
	struct mpssas_softc *sassc;

	sassc = cam_sim_softc(sim);

	MPS_FUNCTRACE(sassc->sc);
	mps_dprint(sassc->sc, MPS_TRACE, "ccb func_code 0x%x\n",
	    ccb->ccb_h.func_code);
	mtx_assert(&sassc->sc->mps_mtx, MA_OWNED);

	switch (ccb->ccb_h.func_code) {
	/*
	 * XNU: no XPT_PATH_INQ, XPT_GET_TRAN_SETTINGS, XPT_CALC_GEOMETRY or
	 * XPT_SMP_IO: the kext asks the core directly (maxio, link rate).
	 */
	case XPT_RESET_DEV:
		mps_dprint(sassc->sc, MPS_XINFO, "mpssas_action XPT_RESET_DEV\n");
		mpssas_action_resetdev(sassc, ccb);
		return;
	case XPT_RESET_BUS:
	case XPT_ABORT:
	case XPT_TERM_IO:
		mps_dprint(sassc->sc, MPS_XINFO,
		    "mpssas_action faking success for abort or reset\n");
		mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
		break;
	case XPT_SCSI_IO:
		mpssas_action_scsiio(sassc, ccb);
		return;
	case XPT_XNU_TM:	/* XNU: OS-requested task management */
		mpssas_action_xnu_tm(sassc, ccb);
		return;
	default:
		mpssas_set_ccbstatus(ccb, CAM_FUNC_NOTAVAIL);
		break;
	}
	xpt_done(ccb);

}

static void
mpssas_announce_reset(struct mps_softc *sc, uint32_t ac_code,
    target_id_t target_id, lun_id_t lun_id)
{
	path_id_t path_id = cam_sim_path(sc->sassc->sim);
	struct cam_path *path;

	mps_dprint(sc, MPS_XINFO, "%s code %x target %d lun %jx\n", __func__,
	    ac_code, target_id, (uintmax_t)lun_id);

	if (xpt_create_path(&path, NULL, 
		path_id, target_id, lun_id) != CAM_REQ_CMP) {
		mps_dprint(sc, MPS_ERROR, "unable to create path for reset "
			   "notification\n");
		return;
	}

	xpt_async(ac_code, path, NULL);
	xpt_free_path(path);
}

static void 
mpssas_complete_all_commands(struct mps_softc *sc)
{
	struct mps_command *cm;
	int i;
	int completed;

	MPS_FUNCTRACE(sc);
	mtx_assert(&sc->mps_mtx, MA_OWNED);

	/* complete all commands with a NULL reply */
	for (i = 1; i < sc->num_reqs; i++) {
		cm = &sc->commands[i];
		if (cm->cm_state == MPS_CM_STATE_FREE)
			continue;

		cm->cm_state = MPS_CM_STATE_BUSY;
		cm->cm_reply = NULL;
		completed = 0;

		/*
		 * XNU: a timed-out SATA identify holds the lent config page
		 * buffer, not malloc'd memory: its completion
		 * (mpssas_ata_id_complete) gives it back.
		 */

		if (cm->cm_flags & MPS_CM_FLAGS_POLLED)
			cm->cm_flags |= MPS_CM_FLAGS_COMPLETE;

		if (cm->cm_complete != NULL) {
			mpssas_log_command(cm, MPS_RECOVERY,
			    "completing cm %p state %x ccb %p for diag reset\n",
			    cm, cm->cm_state, cm->cm_ccb);

			cm->cm_complete(sc, cm);
			completed = 1;
		} else if (cm->cm_flags & MPS_CM_FLAGS_WAKEUP) {
			mpssas_log_command(cm, MPS_RECOVERY,
			    "waking up cm %p state %x ccb %p for diag reset\n", 
			    cm, cm->cm_state, cm->cm_ccb);
			wakeup(cm);
			completed = 1;
		}

		if ((completed == 0) && (cm->cm_state != MPS_CM_STATE_FREE)) {
			/* this should never happen, but if it does, log */
			mpssas_log_command(cm, MPS_RECOVERY,
			    "cm %p state %x flags 0x%x ccb %p during diag "
			    "reset\n", cm, cm->cm_state, cm->cm_flags,
			    cm->cm_ccb);
		}
	}

	sc->io_cmds_active = 0;
}

void
mpssas_handle_reinit(struct mps_softc *sc)
{
	int i;

	/* Go back into startup mode and freeze the simq, so that CAM
	 * doesn't send any commands until after we've rediscovered all
	 * targets and found the proper device handles for them.
	 *
	 * After the reset, portenable will trigger discovery, and after all
	 * discovery-related activities have finished, the simq will be
	 * released.
	 */
	mps_dprint(sc, MPS_INIT, "%s startup\n", __func__);
	sc->sassc->flags |= MPSSAS_IN_STARTUP;
	sc->sassc->flags |= MPSSAS_IN_DISCOVERY;
	/* XNU: an abandoned reinit (system sleep) still holds the startup */
	if (sc->xnu.reinit_owed)
		sc->xnu.reinit_owed = 0;
	else
		mpssas_startup_increment(sc->sassc);
	mps_xnu_readd_cancel(sc, 0);	/* XNU: the rediscovery adds them */
	mps_xnu_reinit_targets(sc);	/* XNU: D8, identity kept */

	/* notify CAM of a bus reset */
	mpssas_announce_reset(sc, AC_BUS_RESET, CAM_TARGET_WILDCARD, 
	    CAM_LUN_WILDCARD);

	/* complete and cleanup after all outstanding commands */
	mpssas_complete_all_commands(sc);

	mps_dprint(sc, MPS_INIT,
	    "%s startup %u after command completion\n", __func__,
	    sc->sassc->startup_refcount);

	/* zero all the target handles, since they may change after the
	 * reset, and we have to rediscover all the targets and use the new
	 * handles.  
	 */
	for (i = 0; i < sc->sassc->maxtargets; i++) {
		if (sc->sassc->targets[i].outstanding != 0)
			mps_dprint(sc, MPS_INIT, "target %u outstanding %u\n", 
			    i, sc->sassc->targets[i].outstanding);
		sc->sassc->targets[i].handle = 0x0;
		sc->sassc->targets[i].exp_dev_handle = 0x0;
		sc->sassc->targets[i].outstanding = 0;
		sc->sassc->targets[i].flags = MPSSAS_TARGET_INDIAGRESET;
	}
}

static void
mpssas_tm_timeout(void *data)
{
	struct mps_command *tm = data;
	struct mps_softc *sc = tm->cm_sc;
	union ccb *ccb;		/* XNU */

	mtx_assert(&sc->mps_mtx, MA_OWNED);

	mpssas_log_command(tm, MPS_INFO|MPS_RECOVERY,
	    "task mgmt %p timed out\n", tm);

	KASSERT(tm->cm_state == MPS_CM_STATE_INQUEUE,
	    ("command not inqueue, state = %u\n", tm->cm_state));

	tm->cm_state = MPS_CM_STATE_BUSY;
	/*
	 * XNU: the IOC still owns the TM until the reinit: a reply that
	 * arrives meanwhile is counted as late and ignored (mps_intr_locked).
	 * An OS-requested TM completes its ccb now; the reinit frees the TM.
	 * mps_reinit() sleeps, so it runs on the reinit taskqueue.
	 */
	tm->cm_flags |= MPS_CM_FLAGS_XNU_TM_TIMEDOUT;
	sc->xnu.stats.tm_timeouts++;
	if (tm->cm_complete == mpssas_xnu_tm_complete &&
	    (ccb = tm->cm_complete_data) != NULL) {
		tm->cm_complete_data = NULL;
		mpssas_set_ccbstatus(ccb, CAM_CMD_TIMEOUT);
		xpt_done(ccb);
	}
	mps_xnu_request_reinit(sc, "task management timeout");
}

static void
mpssas_logical_unit_reset_complete(struct mps_softc *sc, struct mps_command *tm)
{
	MPI2_SCSI_TASK_MANAGE_REPLY *reply;
	unsigned int cm_count = 0;
	struct mps_command *cm;
	struct mpssas_target *targ;

	callout_stop(&tm->cm_callout);

	reply = (MPI2_SCSI_TASK_MANAGE_REPLY *)tm->cm_reply;
	targ = tm->cm_targ;

	/*
	 * Currently there should be no way we can hit this case.  It only
	 * happens when we have a failure to allocate chain frames, and
	 * task management commands don't have S/G lists.
	 * XXXSL So should it be an assertion?
	 */
	if ((tm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		mps_dprint(sc, MPS_RECOVERY|MPS_ERROR,
		    "%s: cm_flags = %#x for LUN reset! "
		   "This should not happen!\n", __func__, tm->cm_flags);
		mpssas_free_tm(sc, tm);
		return;
	}

	if (reply == NULL) {
		mps_dprint(sc, MPS_RECOVERY, "NULL reset reply for tm %p\n",
		    tm);
		if (MPS_XNU_RESETTING(sc)) {	/* XNU: or gone, detaching */
			/* this completion was due to a reset, just cleanup */
			mps_dprint(sc, MPS_RECOVERY, "Hardware undergoing "
			    "reset, ignoring NULL LUN reset reply\n");
			targ->tm = NULL;
			mpssas_free_tm(sc, tm);
		}
		else {
			/* we should have gotten a reply. */
			mps_dprint(sc, MPS_INFO|MPS_RECOVERY, "NULL reply on "
			    "LUN reset attempt, resetting controller\n");
			mps_xnu_request_reinit(sc, "NULL LUN reset reply");	/* XNU */
		}
		return;
	}
	mps_xnu_tm_done(sc, tm);	/* XNU: statistics */

	mps_dprint(sc, MPS_RECOVERY,
	    "logical unit reset status 0x%x code 0x%x count %u\n",
	    le16toh(reply->IOCStatus), le32toh(reply->ResponseCode),
	    le32toh(reply->TerminationCount));

	/*
	 * See if there are any outstanding commands for this LUN.
	 * This could be made more efficient by using a per-LU data
	 * structure of some sort.
	 */
	TAILQ_FOREACH(cm, &targ->commands, cm_link) {
		if (cm->cm_lun == tm->cm_lun)
			cm_count++;
	}

	if (cm_count == 0) {
		mps_dprint(sc, MPS_RECOVERY|MPS_INFO,
		    "Finished recovery after LUN reset for target %u\n",
		    targ->tid);

		mpssas_announce_reset(sc, AC_SENT_BDR, targ->tid, tm->cm_lun);

		/*
		 * We've finished recovery for this logical unit.  check and
		 * see if some other logical unit has a timedout command
		 * that needs to be processed.
		 */
		cm = TAILQ_FIRST(&targ->timedout_commands);
		if (cm) {
			mps_dprint(sc, MPS_INFO|MPS_RECOVERY,
			    "More commands to abort for target %u\n",
			    targ->tid);
			mpssas_send_abort(sc, tm, cm);
		} else {
			sc->xnu.stats.recovered_lun_reset++;	/* XNU */
			targ->tm = NULL;
			mpssas_free_tm(sc, tm);
		}
	} else {
		/*
		 * If we still have commands for this LUN, the reset
		 * effectively failed, regardless of the status reported.
		 * Escalate to a target reset.
		 */
		mps_dprint(sc, MPS_INFO|MPS_RECOVERY,
		    "logical unit reset complete for target %u, but still "
		    "have %u command(s), sending target reset\n", targ->tid,
		    cm_count);
		mpssas_send_reset(sc, tm,
		    MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET);
	}
}

static void
mpssas_target_reset_complete(struct mps_softc *sc, struct mps_command *tm)
{
	MPI2_SCSI_TASK_MANAGE_REPLY *reply;
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	struct mpssas_target *targ;

	callout_stop(&tm->cm_callout);

	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	reply = (MPI2_SCSI_TASK_MANAGE_REPLY *)tm->cm_reply;
	targ = tm->cm_targ;

	/*
	 * Currently there should be no way we can hit this case.  It only
	 * happens when we have a failure to allocate chain frames, and
	 * task management commands don't have S/G lists.
	 */
	if ((tm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		mps_dprint(sc, MPS_ERROR,"%s: cm_flags = %#x for target reset! "
			   "This should not happen!\n", __func__, tm->cm_flags);
		mpssas_free_tm(sc, tm);
		return;
	}

	if (reply == NULL) {
		mps_dprint(sc, MPS_RECOVERY,
		    "NULL target reset reply for tm %p TaskMID %u\n",
		    tm, le16toh(req->TaskMID));
		if (MPS_XNU_RESETTING(sc)) {	/* XNU: or gone, detaching */
			/* this completion was due to a reset, just cleanup */
			mps_dprint(sc, MPS_RECOVERY, "Hardware undergoing "
			    "reset, ignoring NULL target reset reply\n");
			targ->tm = NULL;
			mpssas_free_tm(sc, tm);
		} else {
			/* we should have gotten a reply. */
			mps_dprint(sc, MPS_INFO|MPS_RECOVERY, "NULL reply on "
			    "target reset attempt, resetting controller\n");
			mps_xnu_request_reinit(sc, "NULL target reset reply");	/* XNU */
		}
		return;
	}
	mps_xnu_tm_done(sc, tm);	/* XNU: statistics */

	mps_dprint(sc, MPS_RECOVERY,
	    "target reset status 0x%x code 0x%x count %u\n",
	    le16toh(reply->IOCStatus), le32toh(reply->ResponseCode),
	    le32toh(reply->TerminationCount));

	if (targ->outstanding == 0) {
		/* we've finished recovery for this target and all
		 * of its logical units.
		 */
		mps_dprint(sc, MPS_RECOVERY|MPS_INFO,
		    "Finished reset recovery for target %u\n", targ->tid);

		mpssas_announce_reset(sc, AC_SENT_BDR, tm->cm_targ->tid,
		    CAM_LUN_WILDCARD);

		sc->xnu.stats.recovered_target_reset++;	/* XNU */
		targ->tm = NULL;
		mpssas_free_tm(sc, tm);
	} else {
		/*
		 * After a target reset, if this target still has
		 * outstanding commands, the reset effectively failed,
		 * regardless of the status reported.  escalate.
		 */
		mps_dprint(sc, MPS_INFO|MPS_RECOVERY,
		    "Target reset complete for target %u, but still have %u "
		    "command(s), resetting controller\n", targ->tid,
		    targ->outstanding);
		/* XNU: on the reinit taskqueue; the TM is freed by the reinit */
		mps_xnu_request_reinit(sc, "target reset did not clear the target");
	}
}

#define MPS_RESET_TIMEOUT 30

int
mpssas_send_reset(struct mps_softc *sc, struct mps_command *tm, uint8_t type)
{
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	struct mpssas_target *target;
	int err;

	target = tm->cm_targ;
	if (target->handle == 0) {
		mps_dprint(sc, MPS_ERROR,"%s null devhandle for target_id %d\n",
		    __func__, target->tid);
		return -1;
	}

	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	req->DevHandle = htole16(target->handle);
	req->TaskType = type;

	if (type == MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET) {
		/* XXX Need to handle invalid LUNs */
		MPS_SET_LUN(req->LUN, tm->cm_lun);
		tm->cm_targ->logical_unit_resets++;
		sc->xnu.stats.lun_resets++;	/* XNU */
		mps_dprint(sc, MPS_RECOVERY|MPS_INFO,
		    "Sending logical unit reset to target %u lun %d\n",
		    target->tid, tm->cm_lun);
		tm->cm_complete = mpssas_logical_unit_reset_complete;
		mpssas_prepare_for_tm(sc, tm, target, tm->cm_lun);
	} else if (type == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET) {
		/*
		 * Target reset method =
		 * 	SAS Hard Link Reset / SATA Link Reset
		 */
		req->MsgFlags = MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET;
		tm->cm_targ->target_resets++;
		sc->xnu.stats.target_resets++;	/* XNU */
		mps_dprint(sc, MPS_RECOVERY|MPS_INFO,
		    "Sending target reset to target %u\n", target->tid);
		tm->cm_complete = mpssas_target_reset_complete;
		mpssas_prepare_for_tm(sc, tm, target, CAM_LUN_WILDCARD);
	} else {
		mps_dprint(sc, MPS_ERROR, "unexpected reset type 0x%x\n", type);
		return -1;
	}

	tm->cm_data = NULL;
	tm->cm_complete_data = (void *)tm;

	callout_reset(&tm->cm_callout, MPS_RESET_TIMEOUT * hz,
	    mpssas_tm_timeout, tm);

	err = mps_map_command(sc, tm);
	if (err)
		mps_dprint(sc, MPS_ERROR|MPS_RECOVERY,
		    "error %d sending reset type %u\n",
		    err, type);

	return err;
}

static void
mpssas_abort_complete(struct mps_softc *sc, struct mps_command *tm)
{
	struct mps_command *cm;
	MPI2_SCSI_TASK_MANAGE_REPLY *reply;
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	struct mpssas_target *targ;

	callout_stop(&tm->cm_callout);

	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	reply = (MPI2_SCSI_TASK_MANAGE_REPLY *)tm->cm_reply;
	targ = tm->cm_targ;

	/*
	 * Currently there should be no way we can hit this case.  It only
	 * happens when we have a failure to allocate chain frames, and
	 * task management commands don't have S/G lists.
	 */
	if ((tm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		mps_dprint(sc, MPS_RECOVERY,
		    "cm_flags = %#x for abort %p TaskMID %u!\n",
		    tm->cm_flags, tm, le16toh(req->TaskMID));
		mpssas_free_tm(sc, tm);
		return;
	}

	if (reply == NULL) {
		mps_dprint(sc, MPS_RECOVERY,
		    "NULL abort reply for tm %p TaskMID %u\n",
		    tm, le16toh(req->TaskMID));
		if (MPS_XNU_RESETTING(sc)) {	/* XNU: or gone, detaching */
			/* this completion was due to a reset, just cleanup */
			mps_dprint(sc, MPS_RECOVERY, "Hardware undergoing "
			    "reset, ignoring NULL abort reply\n");
			targ->tm = NULL;
			mpssas_free_tm(sc, tm);
		} else {
			/* we should have gotten a reply. */
			mps_dprint(sc, MPS_INFO|MPS_RECOVERY, "NULL reply on "
			    "abort attempt, resetting controller\n");
			mps_xnu_request_reinit(sc, "NULL abort reply");	/* XNU */
		}
		return;
	}
	mps_xnu_tm_done(sc, tm);	/* XNU: statistics */

	mps_dprint(sc, MPS_RECOVERY,
	    "abort TaskMID %u status 0x%x code 0x%x count %u\n",
	    le16toh(req->TaskMID),
	    le16toh(reply->IOCStatus), le32toh(reply->ResponseCode),
	    le32toh(reply->TerminationCount));

	cm = TAILQ_FIRST(&tm->cm_targ->timedout_commands);
	if (cm == NULL) {
		/*
		 * If there are no more timedout commands, we're done with
		 * error recovery for this target.
		 */
		mps_dprint(sc, MPS_INFO|MPS_RECOVERY,
		    "Finished abort recovery for target %u\n", targ->tid);

		sc->xnu.stats.recovered_abort++;	/* XNU */
		targ->tm = NULL;
		mpssas_free_tm(sc, tm);
	} else if (le16toh(req->TaskMID) != cm->cm_desc.Default.SMID) {
		/* abort success, but we have more timedout commands to abort */
		mps_dprint(sc, MPS_INFO|MPS_RECOVERY,
		    "Continuing abort recovery for target %u\n", targ->tid);

		mpssas_send_abort(sc, tm, cm);
	} else {
		/* we didn't get a command completion, so the abort
		 * failed as far as we're concerned.  escalate.
		 */
		mps_dprint(sc, MPS_RECOVERY,
		    "Abort failed for target %u, sending logical unit reset\n",
		    targ->tid);

		mpssas_send_reset(sc, tm,
		    MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET);
	}
}

#define MPS_ABORT_TIMEOUT 5

static int
mpssas_send_abort(struct mps_softc *sc, struct mps_command *tm, struct mps_command *cm)
{
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	struct mpssas_target *targ;
	int err;

	targ = cm->cm_targ;
	if (targ->handle == 0) {
		mps_dprint(sc, MPS_ERROR|MPS_RECOVERY,
		    "%s null devhandle for target_id %d\n",
		    __func__, cm->cm_ccb->ccb_h.target_id);
		return -1;
	}

	mpssas_log_command(cm, MPS_RECOVERY|MPS_INFO,
	    "Aborting command %p\n", cm);

	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	req->DevHandle = htole16(targ->handle);
	req->TaskType = MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK;

	/* XXX Need to handle invalid LUNs */
	MPS_SET_LUN(req->LUN, cm->cm_ccb->ccb_h.target_lun);

	req->TaskMID = htole16(cm->cm_desc.Default.SMID);

	tm->cm_data = NULL;
	tm->cm_complete = mpssas_abort_complete;
	tm->cm_complete_data = (void *)tm;
	tm->cm_targ = cm->cm_targ;
	tm->cm_lun = cm->cm_lun;

	callout_reset(&tm->cm_callout, MPS_ABORT_TIMEOUT * hz,
	    mpssas_tm_timeout, tm);

	targ->aborts++;
	sc->xnu.stats.aborts++;	/* XNU */

	mpssas_prepare_for_tm(sc, tm, targ, tm->cm_lun);

	err = mps_map_command(sc, tm);
	if (err)
		mps_dprint(sc, MPS_ERROR|MPS_RECOVERY,
		    "error %d sending abort for cm %p SMID %u\n",
		    err, cm, req->TaskMID);
	return err;
}

static void
mpssas_scsiio_timeout(void *data)
{
	sbintime_t elapsed, now;
	union ccb *ccb;
	struct mps_softc *sc;
	struct mps_command *cm;
	struct mpssas_target *targ;

	cm = (struct mps_command *)data;
	sc = cm->cm_sc;
	ccb = cm->cm_ccb;
	now = sbinuptime();

	MPS_FUNCTRACE(sc);
	mtx_assert(&sc->mps_mtx, MA_OWNED);

	mps_dprint(sc, MPS_XINFO|MPS_RECOVERY, "Timeout checking cm %p\n", cm);

	/*
	 * Run the interrupt handler to make sure it's not pending.  This
	 * isn't perfect because the command could have already completed
	 * and been re-used, though this is unlikely.
	 */
	mps_intr_locked(sc);
	if (cm->cm_flags & MPS_CM_FLAGS_ON_RECOVERY) {
		mpssas_log_command(cm, MPS_XINFO,
		    "SCSI command %p almost timed out\n", cm);
		return;
	}

	if (cm->cm_ccb == NULL) {
		mps_dprint(sc, MPS_ERROR, "command timeout with NULL ccb\n");
		return;
	}

	targ = cm->cm_targ;
	targ->timeouts++;
	sc->xnu.stats.io_timeouts++;	/* XNU */

	elapsed = now - ccb->ccb_h.qos.sim_data;
	mpssas_log_command(cm, MPS_INFO|MPS_RECOVERY,
	    "Command timeout on target %u(0x%04x) %d set, %d.%d elapsed\n",
	    targ->tid, targ->handle, ccb->ccb_h.timeout,
	    sbintime_getsec(elapsed), elapsed & 0xffffffff);

	/* XXX first, check the firmware state, to see if it's still
	 * operational.  if not, do a diag reset.
	 */
	mpssas_set_ccbstatus(cm->cm_ccb, CAM_CMD_TIMEOUT);
	cm->cm_flags |= MPS_CM_FLAGS_ON_RECOVERY | MPS_CM_FLAGS_TIMEDOUT;
	TAILQ_INSERT_TAIL(&targ->timedout_commands, cm, cm_recovery);

	/* XNU: no TM into a reset in progress; the reinit completes the IO */
	if (sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE))
		return;

	if (targ->tm != NULL) {
		/* target already in recovery, just queue up another
		 * timedout command to be processed later.
		 */
		mps_dprint(sc, MPS_RECOVERY,
		    "queued timedout cm %p for processing by tm %p\n",
		    cm, targ->tm);
	} else if ((targ->tm = mpssas_alloc_tm(sc)) != NULL) {
		mps_dprint(sc, MPS_RECOVERY|MPS_INFO,
		    "Sending abort to target %u for SMID %d\n", targ->tid,
		    cm->cm_desc.Default.SMID);
		mps_dprint(sc, MPS_RECOVERY, "timedout cm %p allocated tm %p\n",
		    cm, targ->tm);
		sc->xnu.stats.recoveries++;	/* XNU */

		/* start recovery by aborting the first timedout command */
		mpssas_send_abort(sc, targ->tm, cm);
	} else {
		/* XXX queue this target up for recovery once a TM becomes
		 * available.  The firmware only has a limited number of
		 * HighPriority credits for the high priority requests used
		 * for task management, and we ran out.
		 *
		 * Isilon: don't worry about this for now, since we have
		 * more credits than disks in an enclosure, and limit
		 * ourselves to one TM per target for recovery.
		 *
		 * XNU: queued: mpssas_free_tm() starts it when a TM frees
		 * (mpssas_xnu_retry_recovery).
		 */
		mps_dprint(sc, MPS_ERROR|MPS_RECOVERY,
		    "timedout cm %p failed to allocate a tm\n", cm);
	}

}

/*
 * XNU: the OS timer of a task fired (IOSCSIParallelFamily HandleTimeout).
 * A ccb still held by the thin CAM never reached the IOC: complete it with
 * CAM_CMD_TIMEOUT.  A posted one goes through mpssas_scsiio_timeout (the
 * recovery completes it later).  ENOENT: not ours (already completed).
 */
int
mps_xnu_ccb_timeout(struct mps_softc *sc, union ccb *ccb)
{
	struct mps_command *cm;

	mtx_assert(&sc->mps_mtx, MA_OWNED);
	if (cam_xnu_cancel_held(sc->sassc->sim, ccb, CAM_CMD_TIMEOUT)) {
		sc->xnu.stats.held_timeouts++;
		mps_dprint(sc, MPS_RECOVERY, "held IO for target %u lun %ju "
		    "timed out before it was sent\n", ccb->ccb_h.target_id,
		    (uintmax_t)ccb->ccb_h.target_lun);
		return (0);
	}
	cm = ccb->ccb_h.spriv_ptr1;
	if (cm == NULL || cm->cm_ccb != ccb ||
	    cm->cm_state == MPS_CM_STATE_FREE)
		return (ENOENT);
	/* a completion may be waiting on the reply queue */
	mps_intr_locked(sc);
	if (cm->cm_ccb != ccb || cm->cm_state == MPS_CM_STATE_FREE) {
		mps_dprint(sc, MPS_XINFO|MPS_RECOVERY, "IO for target %u "
		    "completed just before its timeout\n",
		    ccb->ccb_h.target_id);
		return (0);
	}
	callout_stop(&cm->cm_callout);
	mpssas_scsiio_timeout(cm);
	return (0);
}

/*
 * XNU: IO killed by our own reset (reinit, sleep) is re-sent from the held
 * queue, a few times at most, as cam_periph_error retries CAM_SCSI_BUS_RESET
 * without counting it.  The OS would retry only READ/WRITE, spending one of
 * IOBlockStorageServices' retries (D20b); anything else (SYNCHRONIZE CACHE,
 * pass-through) would fail upward.  The drive never saw the command, so a
 * replayed WRITE is what CAM does too.
 */
static int
mpssas_xnu_reset_requeue(struct mps_softc *sc, union ccb *ccb)
{
	uint8_t op;

	if (sc->xnu.shutdown || ccb->ccb_h.func_code != XPT_SCSI_IO ||
	    ccb->ccb_h.xnu_resets >= MPS_XNU_RESET_REQUEUES)
		return (0);
	op = (ccb->ccb_h.flags & CAM_CDB_POINTER) ?
	    ccb->csio.cdb_io.cdb_ptr[0] : ccb->csio.cdb_io.cdb_bytes[0];
	ccb->ccb_h.xnu_resets++;
	sc->xnu.stats.reset_requeues++;
	mps_dprint(sc, MPS_RECOVERY, "%s for target %u killed by a reset: "
	    "re-sent (%d of %d)\n", mps_xnu_cdb_str(op),
	    ccb->ccb_h.target_id, ccb->ccb_h.xnu_resets,
	    MPS_XNU_RESET_REQUEUES);
	return (1);
}

static void
mpssas_action_scsiio(struct mpssas_softc *sassc, union ccb *ccb)
{
	MPI2_SCSI_IO_REQUEST *req;
	struct ccb_scsiio *csio;
	struct mps_softc *sc;
	struct mpssas_target *targ;
	struct mps_command *cm;
	uint32_t mpi_control;
	int error;	/* XNU */

	sc = sassc->sc;
	MPS_FUNCTRACE(sc);
	mtx_assert(&sc->mps_mtx, MA_OWNED);

	csio = &ccb->csio;
	/*
	 * XNU: unplugged or disabled: fail, don't touch the registers.  After
	 * the shutdown stop nothing would ever release a held ccb.
	 */
	if ((sc->mps_flags & MPS_FLAGS_XNU_GONE) || sc->xnu.shutdown) {
		mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		xpt_done(ccb);
		return;
	}
	/* XNU: the OS may address any id: no KASSERT */
	if (csio->ccb_h.target_id >= sassc->maxtargets) {
		mpssas_set_ccbstatus(ccb, CAM_TID_INVALID);
		xpt_done(ccb);
		return;
	}
	targ = &sassc->targets[csio->ccb_h.target_id];
	mps_dprint(sc, MPS_TRACE, "ccb %p target flag %x\n", ccb, targ->flags);
	if (targ->handle == 0x0) {
		/*
		 * XNU: requeue only while discovery still holds the SIM queue;
		 * a target that did not come back after a reset is gone (the
		 * thin CAM releases the freeze at once, FreeBSD's periph later).
		 */
		if ((targ->flags & MPSSAS_TARGET_INDIAGRESET) &&
		    (sassc->flags & MPSSAS_IN_STARTUP)) {
			mps_dprint(sc, MPS_ERROR,
			    "%s NULL handle for target %u in diag reset freezing queue\n",
			    __func__, csio->ccb_h.target_id);
			ccb->ccb_h.status = CAM_REQUEUE_REQ | CAM_DEV_QFRZN;
			xpt_freeze_devq(ccb->ccb_h.path, 1);
			xpt_done(ccb);
			return;
		}
		mps_dprint(sc, MPS_ERROR, "%s NULL handle for target %u\n",
		    __func__, csio->ccb_h.target_id);
		mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		xpt_done(ccb);
		return;
	}
	if (targ->flags & MPS_TARGET_FLAGS_RAID_COMPONENT) {
		mps_dprint(sc, MPS_ERROR, "%s Raid component no SCSI IO "
		    "supported %u\n", __func__, csio->ccb_h.target_id);
		mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		xpt_done(ccb);
		return;
	}
	/*
	 * Sometimes, it is possible to get a command that is not "In
	 * Progress" and was actually aborted by the upper layer.  Check for
	 * this here and complete the command without error.
	 */
	if (mpssas_get_ccbstatus(ccb) != CAM_REQ_INPROG) {
		mps_dprint(sc, MPS_TRACE, "%s Command is not in progress for "
		    "target %u\n", __func__, csio->ccb_h.target_id);
		xpt_done(ccb);
		return;
	}
	/*
	 * If devinfo is 0 this will be a volume.  In that case don't tell CAM
	 * that the volume has timed out.  We want volumes to be enumerated
	 * until they are deleted/removed, not just failed. In either event,
	 * we're removing the target due to a firmware event telling us
	 * the device is now gone (as opposed to some transient event). Since
	 * we're opting to remove failed devices from the OS's view, we need
	 * to propagate that status up the stack.
	 */
	if (targ->flags & MPSSAS_TARGET_INREMOVAL) {
		if (targ->devinfo == 0)
			mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
		else
			mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		xpt_done(ccb);
		return;
	}

	if ((sc->mps_flags & MPS_FLAGS_SHUTDOWN) != 0) {
		mps_dprint(sc, MPS_INFO, "%s shutting down\n", __func__);
		mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		xpt_done(ccb);
		return;
	}

	/*
	 * If target has a reset in progress, the devq should be frozen.
	 * Geting here we likely hit a race, so just requeue.
	 */
	if (targ->flags & MPSSAS_TARGET_INRESET) {
		ccb->ccb_h.status = CAM_REQUEUE_REQ | CAM_DEV_QFRZN;
		mps_dprint(sc, MPS_INFO, "%s: Freezing devq for target ID %d\n",
		    __func__, targ->tid);
		xpt_freeze_devq(ccb->ccb_h.path, 1);
		xpt_done(ccb);
		return;
	}

	cm = mps_alloc_command(sc);
	/* XNU: also while a requested reinit has not started yet, or asleep */
	if (cm == NULL || (sc->mps_flags & MPS_FLAGS_DIAGRESET) ||
	    sc->xnu.reinit_pending || sc->xnu.suspending) {
		if (cm != NULL) {
			mps_free_command(sc, cm);
		}
		if ((sassc->flags & MPSSAS_QUEUE_FROZEN) == 0) {
			xpt_freeze_simq(sassc->sim, 1);
			sassc->flags |= MPSSAS_QUEUE_FROZEN;
		}
		ccb->ccb_h.status &= ~CAM_SIM_QUEUED;
		ccb->ccb_h.status |= CAM_REQUEUE_REQ;
		xpt_done(ccb);
		return;
	}

	req = (MPI2_SCSI_IO_REQUEST *)cm->cm_req;
	bzero(req, sizeof(*req));
	req->DevHandle = htole16(targ->handle);
	req->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
	req->MsgFlags = 0;
	req->SenseBufferLowAddress = htole32(cm->cm_sense_busaddr);
	req->SenseBufferLength = MPS_SENSE_LEN;
	req->SGLFlags = 0;
	req->ChainOffset = 0;
	req->SGLOffset0 = 24;	/* 32bit word offset to the SGL */
	req->SGLOffset1= 0;
	req->SGLOffset2= 0;
	req->SGLOffset3= 0;
	req->SkipCount = 0;
	req->DataLength = htole32(csio->dxfer_len);
	req->BidirectionalDataLength = 0;
	req->IoFlags = htole16(csio->cdb_len);
	req->EEDPFlags = 0;

	/* Note: BiDirectional transfers are not supported */
	switch (csio->ccb_h.flags & CAM_DIR_MASK) {
	case CAM_DIR_IN:
		mpi_control = MPI2_SCSIIO_CONTROL_READ;
		cm->cm_flags |= MPS_CM_FLAGS_DATAIN;
		break;
	case CAM_DIR_OUT:
		mpi_control = MPI2_SCSIIO_CONTROL_WRITE;
		cm->cm_flags |= MPS_CM_FLAGS_DATAOUT;
		break;
	case CAM_DIR_NONE:
	default:
		mpi_control = MPI2_SCSIIO_CONTROL_NODATATRANSFER;
		break;
	}

	if (csio->cdb_len == 32)
                mpi_control |= 4 << MPI2_SCSIIO_CONTROL_ADDCDBLEN_SHIFT;
	/*
	 * It looks like the hardware doesn't require an explicit tag
	 * number for each transaction.  SAM Task Management not supported
	 * at the moment.
	 */
	switch (csio->tag_action) {
	case MSG_HEAD_OF_Q_TAG:
		mpi_control |= MPI2_SCSIIO_CONTROL_HEADOFQ;
		break;
	case MSG_ORDERED_Q_TAG:
		mpi_control |= MPI2_SCSIIO_CONTROL_ORDEREDQ;
		break;
	case MSG_ACA_TASK:
		mpi_control |= MPI2_SCSIIO_CONTROL_ACAQ;
		break;
	case CAM_TAG_ACTION_NONE:
	case MSG_SIMPLE_Q_TAG:
	default:
		mpi_control |= MPI2_SCSIIO_CONTROL_SIMPLEQ;
		break;
	}
	mpi_control |= (csio->priority << MPI2_SCSIIO_CONTROL_TASKPRI_SHIFT) &
	    MPI2_SCSIIO_CONTROL_TASKPRI_MASK;
	/* XNU: no mapping table, no TLR (Transport Layer Retries) bits */
	req->Control = htole32(mpi_control);
	if (MPS_SET_LUN(req->LUN, csio->ccb_h.target_lun) != 0) {
		mps_free_command(sc, cm);
		mpssas_set_ccbstatus(ccb, CAM_LUN_INVALID);
		xpt_done(ccb);
		return;
	}

	/* XNU: FreeBSD's CAM bounds cdb_len before the SIM sees it */
	if (csio->cdb_len > ((csio->ccb_h.flags & CAM_CDB_POINTER) ?
	    sizeof(req->CDB.CDB32) : IOCDBLEN)) {
		mps_free_command(sc, cm);
		mpssas_set_ccbstatus(ccb, CAM_REQ_INVALID);
		xpt_done(ccb);
		return;
	}
	if (csio->ccb_h.flags & CAM_CDB_POINTER)
		bcopy(csio->cdb_io.cdb_ptr, &req->CDB.CDB32[0], csio->cdb_len);
	else
		bcopy(csio->cdb_io.cdb_bytes, &req->CDB.CDB32[0],csio->cdb_len);
	req->IoFlags = htole16(csio->cdb_len);

	/*
	 * XNU: EEDP (T10 PI) not ported: it needs CAM's advinfo (READ
	 * CAPACITY 16 protection bits).  sc->eedp_enabled is set from
	 * IOCCapabilities but unused (D28).
	 */

	cm->cm_length = csio->dxfer_len;
	if (cm->cm_length != 0) {
		cm->cm_data = ccb;
		cm->cm_flags |= MPS_CM_FLAGS_USE_CCB;
	} else {
		cm->cm_data = NULL;
	}
	cm->cm_sge = &req->SGL;
	cm->cm_sglsize = (32 - 24) * 4;
	cm->cm_desc.SCSIIO.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_SCSI_IO;
	cm->cm_desc.SCSIIO.DevHandle = htole16(targ->handle);
	cm->cm_complete = mpssas_scsiio_complete;
	cm->cm_complete_data = ccb;
	cm->cm_targ = targ;
	cm->cm_lun = csio->ccb_h.target_lun;
	cm->cm_ccb = ccb;
	ccb->ccb_h.spriv_ptr1 = cm;	/* XNU: mps_xnu_ccb_timeout */

	/* XNU: no WarpDrive (direct drive IO) */

	csio->ccb_h.qos.sim_data = sbinuptime();
	/* XNU: timeout 0: the OS times the task (mps_xnu_ccb_timeout) */
	if (ccb->ccb_h.timeout != 0)
		callout_reset_sbt(&cm->cm_callout, SBT_1MS * ccb->ccb_h.timeout,
		    0, mpssas_scsiio_timeout, cm, 0);

	targ->issued++;
	targ->outstanding++;
	TAILQ_INSERT_TAIL(&targ->commands, cm, cm_link);
	ccb->ccb_h.status |= CAM_SIM_QUEUED;

	mpssas_log_command(cm, MPS_XINFO, "%s cm %p ccb %p outstanding %u\n",
	    __func__, cm, ccb, targ->outstanding);

	/*
	 * XNU: the thin CAM refuses a segment list the tag cannot take
	 * (EFBIG) or a non-CAM_DATA_SG_PADDR buffer (EINVAL) without calling
	 * mps_data_cb, so the command never reached the IOC: undo and fail.
	 */
	error = mps_map_command(sc, cm);
	if (error == EFBIG || error == EINVAL) {
		callout_stop(&cm->cm_callout);
		targ->issued--;
		targ->outstanding--;
		TAILQ_REMOVE(&targ->commands, cm, cm_link);
		ccb->ccb_h.spriv_ptr1 = NULL;
		mps_dprint(sc, MPS_ERROR, "%s: cannot map %u segments "
		    "(error %d)\n", __func__, csio->sglist_cnt, error);
		mps_free_command(sc, cm);
		mpssas_set_ccbstatus(ccb, (error == EFBIG) ? CAM_REQ_TOO_BIG :
		    CAM_REQ_INVALID);
		xpt_done(ccb);
		return;
	}
	if (cm->cm_ccb == ccb && cm->cm_state == MPS_CM_STATE_INQUEUE)
		mps_xnu_io_start(sc, cm);	/* XNU: trace line */
	return;
}

/**
 * mps_sc_failed_io_info - translated non-succesfull SCSI_IO request
 */
static void
mps_sc_failed_io_info(struct mps_softc *sc, struct ccb_scsiio *csio,
    Mpi2SCSIIOReply_t *mpi_reply)
{
	u32 response_info;
	u8 *response_bytes;
	u16 ioc_status = le16toh(mpi_reply->IOCStatus) &
	    MPI2_IOCSTATUS_MASK;
	u8 scsi_state = mpi_reply->SCSIState;
	u8 scsi_status = mpi_reply->SCSIStatus;
	u32 log_info = le32toh(mpi_reply->IOCLogInfo);
	const char *desc_ioc_state, *desc_scsi_status;

	if (log_info == 0x31170000)
		return;

	desc_ioc_state = mps_describe_table(mps_iocstatus_string,
	    ioc_status);
	desc_scsi_status = mps_describe_table(mps_scsi_status_string,
	    scsi_status);

	mps_dprint(sc, MPS_XINFO, "\thandle(0x%04x), ioc_status(%s)(0x%04x)\n",
	    le16toh(mpi_reply->DevHandle), desc_ioc_state, ioc_status);

	/*
	 *We can add more detail about underflow data here
	 * TO-DO
	 */
	mps_dprint(sc, MPS_XINFO, "\tscsi_status(%s)(0x%02x), "
	    "scsi_state %s\n", desc_scsi_status, scsi_status,	/* XNU: no %b */
	    MPS_XNU_BITS(scsi_state, "\20" "\1AutosenseValid"
	    "\2AutosenseFailed" "\3NoScsiStatus" "\4Terminated"
	    "\5Response InfoValid"));

	/* XNU: the sense data is decoded in the failed-IO line (mps_xnu.c) */

	if (scsi_state & MPI2_SCSI_STATE_RESPONSE_INFO_VALID) {
		response_info = le32toh(mpi_reply->ResponseInfo);
		response_bytes = (u8 *)&response_info;
		mps_dprint(sc, MPS_XINFO, "response code(0x%1x): %s\n",
		    response_bytes[0],
		    mps_describe_table(mps_scsi_taskmgmt_string,
		    response_bytes[0]));
	}
}

static void
mpssas_scsiio_complete(struct mps_softc *sc, struct mps_command *cm)
{
	MPI2_SCSI_IO_REPLY *rep;
	union ccb *ccb;
	struct ccb_scsiio *csio;
	struct mpssas_softc *sassc;
	int dir = 0;
	target_id_t target_id;

	MPS_FUNCTRACE(sc);
	mps_dprint(sc, MPS_TRACE,
	    "cm %p SMID %u ccb %p reply %p outstanding %u\n", cm,
	    cm->cm_desc.Default.SMID, cm->cm_ccb, cm->cm_reply,
	    cm->cm_targ->outstanding);

	callout_stop(&cm->cm_callout);
	mtx_assert(&sc->mps_mtx, MA_OWNED);

	sassc = sc->sassc;
	ccb = cm->cm_complete_data;
	csio = &ccb->csio;
	target_id = csio->ccb_h.target_id;
	rep = (MPI2_SCSI_IO_REPLY *)cm->cm_reply;
	/*
	 * XXX KDM if the chain allocation fails, does it matter if we do
	 * the sync and unload here?  It is simpler to do it in every case,
	 * assuming it doesn't cause problems.
	 */
	if (cm->cm_data != NULL) {
		if (cm->cm_flags & MPS_CM_FLAGS_DATAIN)
			dir = BUS_DMASYNC_POSTREAD;
		else if (cm->cm_flags & MPS_CM_FLAGS_DATAOUT)
			dir = BUS_DMASYNC_POSTWRITE;
		bus_dmamap_sync(sc->buffer_dmat, cm->cm_dmamap, dir);
		bus_dmamap_unload(sc->buffer_dmat, cm->cm_dmamap);
	}

	cm->cm_targ->completed++;
	cm->cm_targ->outstanding--;
	TAILQ_REMOVE(&cm->cm_targ->commands, cm, cm_link);
	ccb->ccb_h.status &= ~(CAM_STATUS_MASK | CAM_SIM_QUEUED);
	ccb->ccb_h.spriv_ptr1 = NULL;	/* XNU */

	if (cm->cm_flags & MPS_CM_FLAGS_ON_RECOVERY) {
		TAILQ_REMOVE(&cm->cm_targ->timedout_commands, cm, cm_recovery);
		KASSERT(cm->cm_state == MPS_CM_STATE_BUSY,
		    ("Not busy for CM_FLAGS_TIMEDOUT: %u\n", cm->cm_state));
		cm->cm_flags &= ~MPS_CM_FLAGS_ON_RECOVERY;
		if (cm->cm_reply != NULL)
			mpssas_log_command(cm, MPS_RECOVERY,
			    "completed timedout cm %p ccb %p during recovery "
			    "ioc %x scsi %x state %x xfer %u\n",
			    cm, cm->cm_ccb, le16toh(rep->IOCStatus),
			    rep->SCSIStatus, rep->SCSIState,
			    le32toh(rep->TransferCount));
		else
			mpssas_log_command(cm, MPS_RECOVERY,
			    "completed timedout cm %p ccb %p during recovery\n",
			    cm, cm->cm_ccb);
	} else if (cm->cm_targ->tm != NULL) {
		if (cm->cm_reply != NULL)
			mpssas_log_command(cm, MPS_RECOVERY,
			    "completed cm %p ccb %p during recovery "
			    "ioc %x scsi %x state %x xfer %u\n",
			    cm, cm->cm_ccb, le16toh(rep->IOCStatus),
			    rep->SCSIStatus, rep->SCSIState,
			    le32toh(rep->TransferCount));
		else
			mpssas_log_command(cm, MPS_RECOVERY,
			    "completed cm %p ccb %p during recovery\n",
			    cm, cm->cm_ccb);
	} else if ((sc->mps_flags & MPS_FLAGS_DIAGRESET) != 0) {
		mpssas_log_command(cm, MPS_RECOVERY,
		    "reset completed cm %p ccb %p\n",
		    cm, cm->cm_ccb);
	}

	if ((cm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		/*
		 * We ran into an error after we tried to map the command,
		 * so we're getting a callback without queueing the command
		 * to the hardware.  So we set the status here, and it will
		 * be retained below.  We'll go through the "fast path",
		 * because there can be no reply when we haven't actually
		 * gone out to the hardware.
		 */
		mpssas_set_ccbstatus(ccb, CAM_REQUEUE_REQ);

		/*
		 * Currently the only error included in the mask is
		 * MPS_CM_FLAGS_CHAIN_FAILED, which means we're out of
		 * chain frames.  We need to freeze the queue until we get
		 * a command that completed without this error, which will
		 * hopefully have some chain frames attached that we can
		 * use.  If we wanted to get smarter about it, we would
		 * only unfreeze the queue in this condition when we're
		 * sure that we're getting some chain frames back.  That's
		 * probably unnecessary.
		 */
		if ((sassc->flags & MPSSAS_QUEUE_FROZEN) == 0) {
			xpt_freeze_simq(sassc->sim, 1);
			sassc->flags |= MPSSAS_QUEUE_FROZEN;
			mps_dprint(sc, MPS_XINFO, "Error sending command, "
				   "freezing SIM queue\n");
		}
	}

	/* XNU: no START STOP UNIT at shutdown (SSU_refcount) */

	/* Take the fast path to completion */
	if (cm->cm_reply == NULL) {
		if (mpssas_get_ccbstatus(ccb) == CAM_REQ_INPROG) {
			/* XNU: unplugged, disabled or detaching */
			if (sc->mps_flags & (MPS_FLAGS_XNU_GONE |
			    MPS_FLAGS_SHUTDOWN))
				mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
			else if ((sc->mps_flags & MPS_FLAGS_DIAGRESET) != 0 &&
			    mpssas_xnu_reset_requeue(sc, ccb))
				mpssas_set_ccbstatus(ccb, CAM_REQUEUE_REQ);
			else if ((sc->mps_flags & MPS_FLAGS_DIAGRESET) != 0)
				mpssas_set_ccbstatus(ccb, CAM_SCSI_BUS_RESET);
			else {
				mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
				ccb->csio.scsi_status = SCSI_STATUS_OK;
			}
			if (sassc->flags & MPSSAS_QUEUE_FROZEN) {
				ccb->ccb_h.status |= CAM_RELEASE_SIMQ;
				sassc->flags &= ~MPSSAS_QUEUE_FROZEN;
				mps_dprint(sc, MPS_XINFO,
				    "Unfreezing SIM queue\n");
			}
		}

		/*
		 * There are two scenarios where the status won't be
		 * CAM_REQ_CMP.  The first is if MPS_CM_FLAGS_ERROR_MASK is
		 * set, the second is in the MPS_FLAGS_DIAGRESET above.
		 */
		if (mpssas_get_ccbstatus(ccb) != CAM_REQ_CMP) {
			/*
			 * Freeze the dev queue so that commands are
			 * executed in the correct order after error
			 * recovery.
			 */
			ccb->ccb_h.status |= CAM_DEV_QFRZN;
			xpt_freeze_devq(ccb->ccb_h.path, /*count*/ 1);
		}
		mps_xnu_io_done(sc, cm, ccb, NULL);	/* XNU: statistics */
		mps_free_command(sc, cm);
		xpt_done(ccb);
		return;
	}

	mpssas_log_command(cm, MPS_XINFO,
	    "ioc %x scsi %x state %x xfer %u\n",
	    le16toh(rep->IOCStatus), rep->SCSIStatus, rep->SCSIState,
	    le32toh(rep->TransferCount));

	/* XNU: no WarpDrive direct drive IO (MPS_CM_FLAGS_DD_IO) */
	ccb->ccb_h.sim_priv.entries[0].field = 0;

	switch (le16toh(rep->IOCStatus) & MPI2_IOCSTATUS_MASK) {
	case MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN:
		csio->resid = cm->cm_length - le32toh(rep->TransferCount);
		/* FALLTHROUGH */
	case MPI2_IOCSTATUS_SUCCESS:
	case MPI2_IOCSTATUS_SCSI_RECOVERED_ERROR:

		if ((le16toh(rep->IOCStatus) & MPI2_IOCSTATUS_MASK) ==
		    MPI2_IOCSTATUS_SCSI_RECOVERED_ERROR)
			mpssas_log_command(cm, MPS_XINFO, "recovered error\n");

		/* Completion failed at the transport level. */
		if (rep->SCSIState & (MPI2_SCSI_STATE_NO_SCSI_STATUS |
		    MPI2_SCSI_STATE_TERMINATED)) {
			mpssas_set_ccbstatus(ccb, CAM_REQ_CMP_ERR);
			break;
		}

		/* In a modern packetized environment, an autosense failure
		 * implies that there's not much else that can be done to
		 * recover the command.
		 */
		if (rep->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_FAILED) {
			mpssas_set_ccbstatus(ccb, CAM_AUTOSENSE_FAIL);
			break;
		}

		/* XNU: no TLR (no mapping table) */

		/*
		 * Intentionally override the normal SCSI status reporting
		 * for these two cases.  These are likely to happen in a
		 * multi-initiator environment, and we want to make sure that
		 * CAM retries these commands rather than fail them.
		 */
		if ((rep->SCSIStatus == MPI2_SCSI_STATUS_COMMAND_TERMINATED) ||
		    (rep->SCSIStatus == MPI2_SCSI_STATUS_TASK_ABORTED)) {
			mpssas_set_ccbstatus(ccb, CAM_REQ_ABORTED);
			break;
		}

		/* Handle normal status and sense */
		csio->scsi_status = rep->SCSIStatus;
		if (rep->SCSIStatus == MPI2_SCSI_STATUS_GOOD)
			mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
		else
			mpssas_set_ccbstatus(ccb, CAM_SCSI_STATUS_ERROR);

		if (rep->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_VALID) {
			int sense_len, returned_sense_len;

			returned_sense_len = min(le32toh(rep->SenseCount),
			    sizeof(struct scsi_sense_data));
			if (returned_sense_len < ccb->csio.sense_len)
				ccb->csio.sense_resid = ccb->csio.sense_len -
					returned_sense_len;
			else
				ccb->csio.sense_resid = 0;

			sense_len = min(returned_sense_len,
			    ccb->csio.sense_len - ccb->csio.sense_resid);
			bzero(&ccb->csio.sense_data,
			      sizeof(ccb->csio.sense_data));
			bcopy(cm->cm_sense, &ccb->csio.sense_data, sense_len);
			ccb->ccb_h.status |= CAM_AUTOSNS_VALID;
		}

		/* XNU: no TLR, no SATA START STOP UNIT (no mapping table) */
		break;
	case MPI2_IOCSTATUS_SCSI_INVALID_DEVHANDLE:
	case MPI2_IOCSTATUS_SCSI_DEVICE_NOT_THERE:
		/*
		 * If devinfo is 0 this will be a volume.  In that case don't
		 * tell CAM that the volume is not there.  We want volumes to
		 * be enumerated until they are deleted/removed, not just
		 * failed.
		 */
		if (cm->cm_targ->devinfo == 0)
			mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
		else {
			mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
			/* XNU: cam_periph_error's AC_LOST_DEVICE (D20c) */
			if ((cm->cm_targ->flags & MPSSAS_TARGET_INREMOVAL) == 0)
				mpssas_rescan_target(sc, cm->cm_targ);
		}
		break;
	case MPI2_IOCSTATUS_INVALID_SGL:
		mps_print_scsiio_cmd(sc, cm);
		mpssas_set_ccbstatus(ccb, CAM_UNREC_HBA_ERROR);
		break;
	case MPI2_IOCSTATUS_SCSI_TASK_TERMINATED:
		/*
		 * This is one of the responses that comes back when an I/O
		 * has been aborted.  If it is because of a timeout that we
		 * initiated, just set the status to CAM_CMD_TIMEOUT.
		 * Otherwise set it to CAM_REQ_ABORTED.  The effect on the
		 * command is the same (it gets retried, subject to the
		 * retry counter), the only difference is what gets printed
		 * on the console.
		 */
		if (cm->cm_flags & MPS_CM_FLAGS_TIMEDOUT)
			mpssas_set_ccbstatus(ccb, CAM_CMD_TIMEOUT);
		else
			mpssas_set_ccbstatus(ccb, CAM_REQ_ABORTED);
		break;
	case MPI2_IOCSTATUS_SCSI_DATA_OVERRUN:
		/* resid is ignored for this condition */
		csio->resid = 0;
		mpssas_set_ccbstatus(ccb, CAM_DATA_RUN_ERR);
		break;
	case MPI2_IOCSTATUS_SCSI_IOC_TERMINATED:
	case MPI2_IOCSTATUS_SCSI_EXT_TERMINATED:
		/*
		 * These can sometimes be transient transport-related
		 * errors, and sometimes persistent drive-related errors.
		 * We used to retry these without decrementing the retry
		 * count by returning CAM_REQUEUE_REQ.  Unfortunately, if
		 * we hit a persistent drive problem that returns one of
		 * these error codes, we would retry indefinitely.  So,
		 * return CAM_REQ_CMP_ERROR so that we decrement the retry
		 * count and avoid infinite retries.  We're taking the
		 * potential risk of flagging false failures in the event
		 * of a topology-related error (e.g. a SAS expander problem
		 * causes a command addressed to a drive to fail), but
		 * avoiding getting into an infinite retry loop. However,
		 * if we get them while were moving a device, we should
		 * fail the request as 'not there' because the device
		 * is effectively gone.
		 */
		if (cm->cm_targ->flags & MPSSAS_TARGET_INREMOVAL)
			mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		else
			mpssas_set_ccbstatus(ccb, CAM_REQ_CMP_ERR);
		mps_dprint(sc, MPS_INFO,
		    "Controller reported %s tgt %u SMID %u loginfo %x%s\n",
		    mps_describe_table(mps_iocstatus_string,
		    le16toh(rep->IOCStatus) & MPI2_IOCSTATUS_MASK),
		    target_id, cm->cm_desc.Default.SMID,
		    le32toh(rep->IOCLogInfo),
		    (cm->cm_targ->flags & MPSSAS_TARGET_INREMOVAL) ? " departing" : "");
		mps_dprint(sc, MPS_XINFO,
		    "SCSIStatus %x SCSIState %x xfercount %u\n",
		    rep->SCSIStatus, rep->SCSIState,
		    le32toh(rep->TransferCount));
		break;
	case MPI2_IOCSTATUS_INVALID_FUNCTION:
	case MPI2_IOCSTATUS_INTERNAL_ERROR:
	case MPI2_IOCSTATUS_INVALID_VPID:
	case MPI2_IOCSTATUS_INVALID_FIELD:
	case MPI2_IOCSTATUS_INVALID_STATE:
	case MPI2_IOCSTATUS_OP_STATE_NOT_SUPPORTED:
	case MPI2_IOCSTATUS_SCSI_IO_DATA_ERROR:
	case MPI2_IOCSTATUS_SCSI_PROTOCOL_ERROR:
	case MPI2_IOCSTATUS_SCSI_RESIDUAL_MISMATCH:
	case MPI2_IOCSTATUS_SCSI_TASK_MGMT_FAILED:
	default:
		mpssas_log_command(cm, MPS_XINFO,
		    "completed ioc %x loginfo %x scsi %x state %x xfer %u\n",
		    le16toh(rep->IOCStatus), le32toh(rep->IOCLogInfo),
		    rep->SCSIStatus, rep->SCSIState,
		    le32toh(rep->TransferCount));
		csio->resid = cm->cm_length;
		mpssas_set_ccbstatus(ccb, CAM_REQ_CMP_ERR);
		break;
	}

	mps_sc_failed_io_info(sc,csio,rep);
	mps_xnu_io_done(sc, cm, ccb, rep);	/* XNU: statistics, error line */

	if (sassc->flags & MPSSAS_QUEUE_FROZEN) {
		ccb->ccb_h.status |= CAM_RELEASE_SIMQ;
		sassc->flags &= ~MPSSAS_QUEUE_FROZEN;
		mps_dprint(sc, MPS_XINFO, "Command completed, "
		    "unfreezing SIM queue\n");
	}

	if (mpssas_get_ccbstatus(ccb) != CAM_REQ_CMP) {
		ccb->ccb_h.status |= CAM_DEV_QFRZN;
		xpt_freeze_devq(ccb->ccb_h.path, /*count*/ 1);
	}

	/*
	 * Check to see if we're removing the device. If so, and this is the
	 * last command on the queue, proceed with the deferred removal of the
	 * device.  Note, for removing a volume, this won't trigger because
	 * pending_remove_tm will be NULL.
	 */
	if (cm->cm_targ->flags & MPSSAS_TARGET_INREMOVAL) {
		if (TAILQ_FIRST(&cm->cm_targ->commands) == NULL &&
		    cm->cm_targ->pending_remove_tm != NULL) {
			mps_dprint(sc, MPS_INFO,
			    "Last pending command complete: starting remove_device target %u handle 0x%04x\n",
			    cm->cm_targ->tid, cm->cm_targ->handle);
			mps_map_command(sc, cm->cm_targ->pending_remove_tm);
			cm->cm_targ->pending_remove_tm = NULL;
		}
	}

	mps_free_command(sc, cm);
	xpt_done(ccb);
}

static void
mpssas_action_resetdev(struct mpssas_softc *sassc, union ccb *ccb)
{
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	struct mps_softc *sc;
	struct mps_command *tm;
	struct mpssas_target *targ;

	MPS_FUNCTRACE(sassc->sc);
	mtx_assert(&sassc->sc->mps_mtx, MA_OWNED);

	KASSERT(ccb->ccb_h.target_id < sassc->maxtargets,
	    ("Target %d out of bounds in XPT_RESET_DEV\n",
	     ccb->ccb_h.target_id));
	sc = sassc->sc;
	tm = mpssas_alloc_tm(sc);
	if (tm == NULL) {
		mps_dprint(sc, MPS_ERROR,
		    "command alloc failure in mpssas_action_resetdev\n");
		mpssas_set_ccbstatus(ccb, CAM_RESRC_UNAVAIL);
		xpt_done(ccb);
		return;
	}

	targ = &sassc->targets[ccb->ccb_h.target_id];
	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	req->DevHandle = htole16(targ->handle);
	req->TaskType = MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET;

	/* SAS Hard Link Reset / SATA Link Reset */
	req->MsgFlags = MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET;

	tm->cm_data = NULL;
	tm->cm_complete = mpssas_resetdev_complete;
	tm->cm_complete_data = ccb;
	tm->cm_targ = targ;

	mpssas_prepare_for_tm(sc, tm, targ, CAM_LUN_WILDCARD);
	mps_map_command(sc, tm);
}

static void
mpssas_resetdev_complete(struct mps_softc *sc, struct mps_command *tm)
{
	MPI2_SCSI_TASK_MANAGE_REPLY *resp;
	union ccb *ccb;

	MPS_FUNCTRACE(sc);
	mtx_assert(&sc->mps_mtx, MA_OWNED);

	resp = (MPI2_SCSI_TASK_MANAGE_REPLY *)tm->cm_reply;
	ccb = tm->cm_complete_data;

	/*
	 * Currently there should be no way we can hit this case.  It only
	 * happens when we have a failure to allocate chain frames, and
	 * task management commands don't have S/G lists.
	 */
	if ((tm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		MPI2_SCSI_TASK_MANAGE_REQUEST *req;

		req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;

		mps_dprint(sc, MPS_ERROR,
			   "%s: cm_flags = %#x for reset of handle %#04x! "
			   "This should not happen!\n", __func__, tm->cm_flags,
			   req->DevHandle);
		mpssas_set_ccbstatus(ccb, CAM_REQ_CMP_ERR);
		goto bailout;
	}

	/* XNU: no reply frame (reset, or the IOC answered without one) */
	if (resp == NULL) {
		mpssas_set_ccbstatus(ccb, MPS_XNU_RESETTING(sc) ?
		    CAM_REQ_ABORTED : CAM_REQ_CMP_ERR);
		if (!MPS_XNU_RESETTING(sc))
			mps_xnu_request_reinit(sc, "NULL target reset reply");
		goto bailout;
	}
	mps_xnu_tm_done(sc, tm);	/* XNU: statistics */

	mps_dprint(sc, MPS_XINFO,
	    "%s: IOCStatus = 0x%x ResponseCode = 0x%x\n", __func__,
	    le16toh(resp->IOCStatus), le32toh(resp->ResponseCode));

	if (le32toh(resp->ResponseCode) == MPI2_SCSITASKMGMT_RSP_TM_COMPLETE) {
		mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
		mpssas_announce_reset(sc, AC_SENT_BDR, tm->cm_targ->tid,
		    CAM_LUN_WILDCARD);
	}
	else
		mpssas_set_ccbstatus(ccb, CAM_REQ_CMP_ERR);

bailout:

	mpssas_free_tm(sc, tm);
	xpt_done(ccb);
}

/*
 * XNU: task management requested by the OS (XPT_XNU_TM): abort task, abort
 * task set, clear task set, clear ACA, LUN reset, target reset.  Built like
 * mpssas_action_resetdev; completes the ccb when the IOC answers, with
 * CAM_CMD_TIMEOUT if it does not within the FreeBSD TM timeouts (the TM is
 * then reclaimed by the reinit mpssas_tm_timeout requests).  An abort of a
 * ccb still held by the thin CAM completes it without involving the IOC.
 */
static void
mpssas_action_xnu_tm(struct mpssas_softc *sassc, union ccb *ccb)
{
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	struct ccb_xnu_tm *xtm = &ccb->xtm;
	struct mps_softc *sc = sassc->sc;
	struct mps_command *tm, *cm = NULL;
	struct mpssas_target *targ;
	uint8_t type = xtm->task_type;

	mtx_assert(&sc->mps_mtx, MA_OWNED);
	xtm->ioc_status = 0;
	xtm->response_code = 0;
	xtm->termination_count = 0;

	if (type != MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK &&
	    type != MPI2_SCSITASKMGMT_TASKTYPE_ABRT_TASK_SET &&
	    type != MPI2_SCSITASKMGMT_TASKTYPE_CLEAR_TASK_SET &&
	    type != MPI2_SCSITASKMGMT_TASKTYPE_CLR_ACA &&
	    type != MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET &&
	    type != MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET) {
		mpssas_set_ccbstatus(ccb, CAM_REQ_INVALID);
		xpt_done(ccb);
		return;
	}
	if (ccb->ccb_h.target_id >= sassc->maxtargets ||
	    (sc->mps_flags & (MPS_FLAGS_XNU_GONE | MPS_FLAGS_SHUTDOWN))) {
		mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		xpt_done(ccb);
		return;
	}
	targ = &sassc->targets[ccb->ccb_h.target_id];
	if (type == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK) {
		if (xtm->abort_ccb == NULL) {
			mpssas_set_ccbstatus(ccb, CAM_REQ_INVALID);
			xpt_done(ccb);
			return;
		}
		if (cam_xnu_cancel_held(sassc->sim, xtm->abort_ccb,
		    CAM_REQ_ABORTED)) {
			mps_dprint(sc, MPS_USER|MPS_RECOVERY, "abort of a held "
			    "IO for target %u: completed without the IOC\n",
			    targ->tid);
			xtm->response_code = MPI2_SCSITASKMGMT_RSP_TM_COMPLETE;
			xtm->termination_count = 1;
			mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
			xpt_done(ccb);
			return;
		}
		cm = xtm->abort_ccb->ccb_h.spriv_ptr1;
		if (cm == NULL || cm->cm_ccb != xtm->abort_ccb ||
		    cm->cm_state != MPS_CM_STATE_INQUEUE) {
			mpssas_set_ccbstatus(ccb, CAM_REQ_INVALID);
			xpt_done(ccb);
			return;
		}
	}
	if (targ->handle == 0) {
		mpssas_set_ccbstatus(ccb, CAM_DEV_NOT_THERE);
		xpt_done(ccb);
		return;
	}
	if ((sc->mps_flags & MPS_FLAGS_DIAGRESET) || sc->xnu.reinit_pending ||
	    sc->xnu.suspending) {
		mpssas_set_ccbstatus(ccb, CAM_BUSY);
		xpt_done(ccb);
		return;
	}
	tm = mpssas_alloc_tm(sc);
	if (tm == NULL) {
		mps_dprint(sc, MPS_ERROR,
		    "command alloc failure in mpssas_action_xnu_tm\n");
		mpssas_set_ccbstatus(ccb, CAM_RESRC_UNAVAIL);
		xpt_done(ccb);
		return;
	}

	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	req->DevHandle = htole16(targ->handle);
	req->TaskType = type;
	tm->cm_lun = ccb->ccb_h.target_lun;
	if (type == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET) {
		/* SAS Hard Link Reset / SATA Link Reset */
		req->MsgFlags = MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET;
	} else if (MPS_SET_LUN(req->LUN, ccb->ccb_h.target_lun) != 0) {
		mpssas_free_tm(sc, tm);
		mpssas_set_ccbstatus(ccb, CAM_LUN_INVALID);
		xpt_done(ccb);
		return;
	}
	if (cm != NULL)
		req->TaskMID = htole16(cm->cm_desc.Default.SMID);

	tm->cm_data = NULL;
	tm->cm_complete = mpssas_xnu_tm_complete;
	tm->cm_complete_data = ccb;
	tm->cm_targ = targ;

	mps_dprint(sc, MPS_USER|MPS_RECOVERY, "OS requested %s for target %u "
	    "lun %ju (SMID %u%s)\n", mps_xnu_tm_type_str(type), targ->tid,
	    (uintmax_t)ccb->ccb_h.target_lun, tm->cm_desc.Default.SMID,
	    cm != NULL ? ", aborting an IO" : "");
	callout_reset(&tm->cm_callout, ((type ==
	    MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK) ? MPS_ABORT_TIMEOUT :
	    MPS_RESET_TIMEOUT) * hz, mpssas_tm_timeout, tm);
	mpssas_prepare_for_tm(sc, tm, targ,
	    (type == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET) ?
	    CAM_LUN_WILDCARD : ccb->ccb_h.target_lun);
	mps_map_command(sc, tm);
}

static void
mpssas_xnu_tm_complete(struct mps_softc *sc, struct mps_command *tm)
{
	MPI2_SCSI_TASK_MANAGE_REPLY *resp;
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	union ccb *ccb;
	uint16_t ioc_status;

	mtx_assert(&sc->mps_mtx, MA_OWNED);
	callout_stop(&tm->cm_callout);
	resp = (MPI2_SCSI_TASK_MANAGE_REPLY *)tm->cm_reply;
	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	ccb = tm->cm_complete_data;	/* NULL: timed out, completed */

	if (resp == NULL) {
		if (ccb != NULL)
			mpssas_set_ccbstatus(ccb, (sc->mps_flags &
			    (MPS_FLAGS_XNU_GONE | MPS_FLAGS_SHUTDOWN)) ?
			    CAM_DEV_NOT_THERE : MPS_XNU_RESETTING(sc) ?
			    CAM_REQ_ABORTED : CAM_REQ_CMP_ERR);
		if (!MPS_XNU_RESETTING(sc))
			mps_xnu_request_reinit(sc, "NULL task management reply");
	} else {
		mps_xnu_tm_done(sc, tm);
		ioc_status = le16toh(resp->IOCStatus) & MPI2_IOCSTATUS_MASK;
		if (ccb != NULL) {
			ccb->xtm.ioc_status = ioc_status;
			ccb->xtm.response_code = resp->ResponseCode;
			ccb->xtm.termination_count =
			    le32toh(resp->TerminationCount);
			if (ioc_status == MPI2_IOCSTATUS_SUCCESS &&
			    (resp->ResponseCode ==
			    MPI2_SCSITASKMGMT_RSP_TM_COMPLETE ||
			    resp->ResponseCode ==
			    MPI2_SCSITASKMGMT_RSP_TM_SUCCEEDED))
				mpssas_set_ccbstatus(ccb, CAM_REQ_CMP);
			else
				mpssas_set_ccbstatus(ccb, CAM_REQ_CMP_ERR);
		}
		if (ioc_status == MPI2_IOCSTATUS_SUCCESS &&
		    (req->TaskType == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET ||
		    req->TaskType ==
		    MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET))
			mpssas_announce_reset(sc, AC_SENT_BDR,
			    tm->cm_targ->tid, req->TaskType ==
			    MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET ?
			    CAM_LUN_WILDCARD : tm->cm_lun);
	}
	mpssas_free_tm(sc, tm);
	if (ccb != NULL)
		xpt_done(ccb);
}

/*
 * XNU: a recovery that found no free high-priority command (FreeBSD: "XXX
 * queue this target up") starts when one frees: mpssas_free_tm.
 */
static void
mpssas_xnu_retry_recovery(struct mps_softc *sc)
{
	struct mpssas_softc *sassc = sc->sassc;
	struct mpssas_target *targ;
	struct mps_command *cm;
	u_int i;

	if (sassc == NULL || MPS_XNU_RESETTING(sc))
		return;
	/* removals first: a removed device needs no recovery */
	for (i = 0; i < sassc->maxtargets; i++) {
		targ = &sassc->targets[i];
		if ((targ->flags & MPSSAS_TARGET_XNU_REMOVE_WAIT) == 0)
			continue;
		if (targ->handle == 0) {
			targ->flags &= ~MPSSAS_TARGET_XNU_REMOVE_WAIT;
			continue;
		}
		if ((cm = mpssas_alloc_tm(sc)) == NULL)
			return;
		mps_dprint(sc, MPS_RECOVERY|MPS_INFO, "starting the queued "
		    "removal of target %u (TM freed)\n", targ->tid);
		mpssas_xnu_send_remove(sc, targ, cm);
	}
	for (i = 0; i < sassc->maxtargets; i++) {
		targ = &sassc->targets[i];
		if (targ->tm != NULL || targ->handle == 0 ||
		    (targ->flags & MPSSAS_TARGET_INREMOVAL) ||
		    (cm = TAILQ_FIRST(&targ->timedout_commands)) == NULL)
			continue;
		if ((targ->tm = mpssas_alloc_tm(sc)) == NULL)
			return;
		mps_dprint(sc, MPS_RECOVERY|MPS_INFO,
		    "Sending abort to target %u for SMID %d (TM freed)\n",
		    targ->tid, cm->cm_desc.Default.SMID);
		sc->xnu.stats.recoveries++;
		mpssas_send_abort(sc, targ->tm, cm);
	}
}

/*
 * XNU: the controller is gone, disabled or being detached (after the IOC
 * was stopped): complete every command and every held ccb, once.
 */
void
mpssas_xnu_fail_all(struct mps_softc *sc)
{

	mtx_assert(&sc->mps_mtx, MA_OWNED);
	if (sc->sassc == NULL || sc->commands == NULL)
		return;
	mpssas_complete_all_commands(sc);
	if (sc->sassc->sim == NULL)
		return;
	/*
	 * Nothing will release the SIM queue now (discovery after a reset,
	 * a completion after a requeue): drop the freezes so new IO reaches
	 * mpssas_action_scsiio and fails there, and fail what is held.
	 */
	sc->sassc->flags &= ~(MPSSAS_IN_STARTUP | MPSSAS_QUEUE_FROZEN);
	sc->sassc->startup_refcount = 0;
	while (sc->sassc->sim->freeze > 0)
		xpt_release_simq(sc->sassc->sim, 1);
	cam_xnu_flush_held(sc->sassc->sim, CAM_DEV_NOT_THERE);
}

/*
 * XNU: system sleep (mps_xnu_suspend): the IOC was stopped by a message unit
 * reset with MPS_FLAGS_DIAGRESET set; complete what it still owned as a diag
 * reset does (IO: CAM_SCSI_BUS_RESET, retried by the OS and held).  The
 * targets keep their handles until the reinit at resume rediscovers them.
 */
void
mpssas_xnu_suspend(struct mps_softc *sc)
{

	mtx_assert(&sc->mps_mtx, MA_OWNED);
	if (sc->sassc == NULL || sc->commands == NULL)
		return;
	mpssas_complete_all_commands(sc);
}

static void
mpssas_poll(struct cam_sim *sim)
{
	struct mpssas_softc *sassc;

	sassc = cam_sim_softc(sim);

	if (sassc->sc->mps_debug & MPS_TRACE) {
		/* frequent debug messages during a panic just slow
		 * everything down too much.
		 */
		mps_printf(sassc->sc, "%s clearing MPS_TRACE\n", __func__);
		sassc->sc->mps_debug &= ~MPS_TRACE;
	}

	mps_intr_locked(sassc->sc);
}

/*
 * Freeze the devq and set the INRESET flag so that no I/O will be sent to
 * the target until the reset has completed.  The CCB holds the path which
 * is used to release the devq.  The devq is released and the CCB is freed
 * when the TM completes.
 * We only need to do this when we're entering reset, not at each time we
 * need to send an abort (which will happen if multiple commands timeout
 * while we're sending the abort). We do not release the queue for each
 * command we complete (just at the end when we free the tm), so freezing
 * it each time doesn't make sense.
 */
void
mpssas_prepare_for_tm(struct mps_softc *sc, struct mps_command *tm,
    struct mpssas_target *target, lun_id_t lun_id)
{
	union ccb *ccb;
	path_id_t path_id;

	/* XNU: statistics, TM duration */
	sc->xnu.stats.tm_sent[((MPI2_SCSI_TASK_MANAGE_REQUEST *)
	    tm->cm_req)->TaskType & 15]++;
	tm->cm_xnu_start_us = mps_os_uptime_us();
	/*
	 * XNU: an escalation (abort -> LUN reset -> target reset) reuses the
	 * TM: keep its freeze and ccb (FreeBSD allocates a new ccb and leaks
	 * the old one).
	 */
	if (tm->cm_ccb != NULL)
		return;

	ccb = xpt_alloc_ccb_nowait();
	if (ccb) {
		path_id = cam_sim_path(sc->sassc->sim);
		/*
		 * XNU: freeze the whole target, not just lun_id: the thin CAM
		 * releases a requeued ccb's own freeze at once, so this one
		 * must hold every IO that would see MPSSAS_TARGET_INRESET.
		 */
		if (xpt_create_path(&ccb->ccb_h.path, xpt_periph, path_id,
		    target->tid, CAM_LUN_WILDCARD) != CAM_REQ_CMP) {
			xpt_free_ccb(ccb);
		} else {
			tm->cm_ccb = ccb;
			tm->cm_targ = target;
			/* XNU: one freeze per TM, released by mpssas_free_tm */
			mps_dprint(sc, MPS_XINFO | MPS_RECOVERY,
			    "%s: Freezing devq for target ID %d\n",
			    __func__, target->tid);
			xpt_freeze_devq(ccb->ccb_h.path, 1);
			target->flags |= MPSSAS_TARGET_INRESET;
		}
	}
}

int
mpssas_startup(struct mps_softc *sc)
{

	/*
	 * Send the port enable message and set the wait_for_port_enable flag.
	 * This flag helps to keep the simq frozen until all discovery events
	 * are processed.
	 */
	sc->wait_for_port_enable = 1;
	mpssas_send_portenable(sc);
	return (0);
}

static int
mpssas_send_portenable(struct mps_softc *sc)
{
	MPI2_PORT_ENABLE_REQUEST *request;
	struct mps_command *cm;

	MPS_FUNCTRACE(sc);

	if ((cm = mps_alloc_command(sc)) == NULL)
		return (EBUSY);
	request = (MPI2_PORT_ENABLE_REQUEST *)cm->cm_req;
	request->Function = MPI2_FUNCTION_PORT_ENABLE;
	request->MsgFlags = 0;
	request->VP_ID = 0;
	cm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	cm->cm_complete = mpssas_portenable_complete;
	cm->cm_data = NULL;
	cm->cm_sge = NULL;

	mps_map_command(sc, cm);
	mps_dprint(sc, MPS_XINFO, 
	    "mps_send_portenable finished cm %p req %p complete %p\n",
	    cm, cm->cm_req, cm->cm_complete);
	return (0);
}

static void
mpssas_portenable_complete(struct mps_softc *sc, struct mps_command *cm)
{
	MPI2_PORT_ENABLE_REPLY *reply;
	struct mpssas_softc *sassc;

	MPS_FUNCTRACE(sc);
	sassc = sc->sassc;

	/*
	 * Currently there should be no way we can hit this case.  It only
	 * happens when we have a failure to allocate chain frames, and
	 * port enable commands don't have S/G lists.
	 */
	if ((cm->cm_flags & MPS_CM_FLAGS_ERROR_MASK) != 0) {
		mps_dprint(sc, MPS_ERROR, "%s: cm_flags = %#x for port enable! "
			   "This should not happen!\n", __func__, cm->cm_flags);
	}

	reply = (MPI2_PORT_ENABLE_REPLY *)cm->cm_reply;
	if (reply == NULL)
		mps_dprint(sc, MPS_FAULT, "Portenable NULL reply\n");
	else if (le16toh(reply->IOCStatus & MPI2_IOCSTATUS_MASK) !=
	    MPI2_IOCSTATUS_SUCCESS)
		mps_dprint(sc, MPS_FAULT, "Portenable failed\n");

	mps_free_command(sc, cm);

	/* XNU: no WarpDrive config pages (mps_wd_config_pages) */

	/*
	 * Done waiting for port enable to complete.  Decrement the refcount.
	 * If refcount is 0, discovery is complete and a rescan of the bus can
	 * take place.  Since the simq was explicitly frozen before port
	 * enable, it must be explicitly released here to keep the
	 * freeze/release count in sync.
	 */
	sc->wait_for_port_enable = 0;
	sc->port_enable_complete = 1;
	wakeup(&sc->port_enable_complete);
	mpssas_startup_decrement(sassc);
}

int
mpssas_check_id(struct mpssas_softc *sassc, int id)
{
	struct mps_softc *sc = sassc->sc;
	char *ids;
	char *name;

	ids = &sc->exclude_ids[0];
	while((name = strsep(&ids, ",")) != NULL) {
		if (name[0] == '\0')
			continue;
		if (strtol(name, NULL, 0) == (long)id)
			return (1);
	}

	return (0);
}

void
mpssas_realloc_targets(struct mps_softc *sc, int maxtargets)
{
	struct mpssas_softc *sassc;
	struct mpssas_lun *lun, *lun_tmp;
	struct mpssas_target *targ;
	int i;

	sassc = sc->sassc;
	/*
	 * The number of targets is based on IOC Facts, so free all of
	 * the allocated LUNs for each target and then the target buffer
	 * itself.
	 */
	for (i=0; i< maxtargets; i++) {
		targ = &sassc->targets[i];
		SLIST_FOREACH_SAFE(lun, &targ->luns, lun_link, lun_tmp) {
			free(lun, M_MPT2);
		}
	}
	free(sassc->targets, M_MPT2);

	sassc->targets = malloc(sizeof(struct mpssas_target) * maxtargets,
	    M_MPT2, M_WAITOK|M_ZERO);
}
