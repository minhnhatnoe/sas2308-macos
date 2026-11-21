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





static int mps_diag_reset(struct mps_softc *sc, int sleep_flag);
static int mps_init_queues(struct mps_softc *sc);
static void mps_resize_queues(struct mps_softc *sc);
static int mps_message_unit_reset(struct mps_softc *sc, int sleep_flag);
static int mps_transition_operational(struct mps_softc *sc);
static int mps_iocfacts_allocate(struct mps_softc *sc, uint8_t attaching);
static void mps_iocfacts_free(struct mps_softc *sc);
static void mps_startup(void *arg);
static int mps_send_iocinit(struct mps_softc *sc);
static int mps_alloc_queues(struct mps_softc *sc);
static int mps_alloc_hw_queues(struct mps_softc *sc);
static int mps_alloc_replies(struct mps_softc *sc);
static int mps_alloc_requests(struct mps_softc *sc);
static int mps_attach_log(struct mps_softc *sc);
static __inline void mps_complete_command(struct mps_softc *sc,
    struct mps_command *cm);
static void mps_dispatch_event(struct mps_softc *sc, uintptr_t data,
    MPI2_EVENT_NOTIFICATION_REPLY *reply);
static void mps_periodic(void *);
static int mps_reregister_events(struct mps_softc *sc);
static void mps_enqueue_request(struct mps_softc *sc, struct mps_command *cm);
static int mps_get_iocfacts(struct mps_softc *sc, MPI2_IOC_FACTS_REPLY *facts);
static int mps_wait_db_ack(struct mps_softc *sc, int timeout, int sleep_flag);

MALLOC_DEFINE(M_MPT2, "mps", "mpt2 driver memory");

/*
 * Do a "Diagnostic Reset" aka a hard reset.  This should get the chip out of
 * any state and back to its initialization state machine.
 */
static char mpt2_reset_magic[] = { 0x00, 0x0f, 0x04, 0x0b, 0x02, 0x07, 0x0d };

/* Added this union to smoothly convert le64toh cm->cm_desc.Words.
 * Compiler only support unint64_t to be passed as argument.
 * Otherwise it will throw below error
 * "aggregate value used where an integer was expected"
 */

typedef union {
        u64 word;
        struct {
                u32 low;
                u32 high;
        } u;
} request_descriptor_t;

/* Rate limit chain-fail messages to 1 per minute */
static struct timeval mps_chainfail_interval = { 60, 0 };

/* 
 * sleep_flag can be either CAN_SLEEP or NO_SLEEP.
 * If this function is called from process context, it can sleep
 * and there is no harm to sleep, in case if this fuction is called
 * from Interrupt handler, we can not sleep and need NO_SLEEP flag set.
 * based on sleep flags driver will call either msleep, pause or DELAY.
 * msleep and pause are of same variant, but pause is used when mps_mtx
 * is not hold by driver.
 *
 */
static int
mps_diag_reset(struct mps_softc *sc,int sleep_flag)
{
	uint32_t reg;
	int i, error, tries = 0;
	uint8_t first_wait_done = FALSE;

	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);

	/* Clear any pending interrupts */
	mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);

	/*
	 * Force NO_SLEEP for threads prohibited to sleep
 	 * e.a Thread from interrupt handler are prohibited to sleep.
 	 */
	if (curthread->td_no_sleeping != 0)
		sleep_flag = NO_SLEEP;

	mps_dprint(sc, MPS_INIT, "sequence start, sleep_flag= %d\n", sleep_flag);

	/* Push the magic sequence */
	error = ETIMEDOUT;
	while (tries++ < 20) {
		if (MPS_XNU_ABANDON(sc))	/* XNU: gone, or detaching */
			return (ENXIO);
		for (i = 0; i < sizeof(mpt2_reset_magic); i++)
			mps_regwrite(sc, MPI2_WRITE_SEQUENCE_OFFSET,
			    mpt2_reset_magic[i]);
		/* wait 100 msec */
		if (mtx_owned(&sc->mps_mtx) && sleep_flag == CAN_SLEEP)
			msleep(&sc->msleep_fake_chan, &sc->mps_mtx, 0,
			    "mpsdiag", hz/10);
		else if (sleep_flag == CAN_SLEEP)
			pause("mpsdiag", hz/10);
		else
			DELAY(100 * 1000);

		reg = mps_regread(sc, MPI2_HOST_DIAGNOSTIC_OFFSET);
		if (reg & MPI2_DIAG_DIAG_WRITE_ENABLE) {
			error = 0;
			break;
		}
	}
	if (error) {
		mps_dprint(sc, MPS_INIT, "sequence failed, error=%d, exit\n",
		    error);
		return (error);
	}

	/* Send the actual reset.  XXX need to refresh the reg? */
	reg |= MPI2_DIAG_RESET_ADAPTER;
	mps_dprint(sc, MPS_INIT, "sequence success, sending reset, reg= 0x%x\n",
		reg);
	mps_regwrite(sc, MPI2_HOST_DIAGNOSTIC_OFFSET, reg);
	sc->xnu.stats.diag_resets++;	/* XNU */

	/* Wait up to 300 seconds in 50ms intervals */
	error = ETIMEDOUT;
	for (i = 0; i < 6000; i++) {
		if (MPS_XNU_ABANDON(sc)) {	/* XNU: gone, or detaching */
			error = ENXIO;
			break;
		}
		/*
		 * Wait 50 msec. If this is the first time through, wait 256
		 * msec to satisfy Diag Reset timing requirements.
		 */
		if (first_wait_done) {
			if (mtx_owned(&sc->mps_mtx) && sleep_flag == CAN_SLEEP)
				msleep(&sc->msleep_fake_chan, &sc->mps_mtx, 0,
				    "mpsdiag", hz/20);
			else if (sleep_flag == CAN_SLEEP)
				pause("mpsdiag", hz/20);
			else
				DELAY(50 * 1000);
		} else {
			DELAY(256 * 1000);
			first_wait_done = TRUE;
		}
		/*
		 * Check for the RESET_ADAPTER bit to be cleared first, then
		 * wait for the RESET state to be cleared, which takes a little
		 * longer.
		 */
		reg = mps_regread(sc, MPI2_HOST_DIAGNOSTIC_OFFSET);
		if (reg & MPI2_DIAG_RESET_ADAPTER) {
			continue;
		}
		reg = mps_regread(sc, MPI2_DOORBELL_OFFSET);
		if ((reg & MPI2_IOC_STATE_MASK) != MPI2_IOC_STATE_RESET) {
			error = 0;
			break;
		}
	}
	if (error) {
		mps_dprint(sc, MPS_INIT, "reset failed, error= %d, exit\n",
		    error);
		return (error);
	}

	mps_regwrite(sc, MPI2_WRITE_SEQUENCE_OFFSET, 0x0);
	mps_dprint(sc, MPS_INIT, "diag reset success, exit\n");

	return (0);
}

static int
mps_message_unit_reset(struct mps_softc *sc, int sleep_flag)
{
	int error;

	MPS_FUNCTRACE(sc);

	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);

	error = 0;
	mps_regwrite(sc, MPI2_DOORBELL_OFFSET,
	    MPI2_FUNCTION_IOC_MESSAGE_UNIT_RESET <<
	    MPI2_DOORBELL_FUNCTION_SHIFT);

	/* XNU: shutdown waits 1 s, not 5 (the halt watchdog is 30 s) */
	if (mps_wait_db_ack(sc, sc->xnu.stop_fast ? 1 : 5, sleep_flag) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT,
		    "Doorbell handshake failed\n");
		error = ETIMEDOUT;
	}

	mps_dprint(sc, MPS_INIT, "%s exit\n", __func__);
	return (error);
}

static int
mps_transition_ready(struct mps_softc *sc)
{
	uint32_t reg, state;
	int error, tries = 0;
	int sleep_flags;

	MPS_FUNCTRACE(sc);
	/* If we are in attach call, do not sleep */
	sleep_flags = (sc->mps_flags & MPS_FLAGS_ATTACH_DONE)
					? CAN_SLEEP:NO_SLEEP;
	error = 0;

	mps_dprint(sc, MPS_INIT, "%s entered, sleep_flags= %d\n",
	   __func__, sleep_flags);

	while (tries++ < 1200) {
		if (MPS_XNU_ABANDON(sc)) {	/* XNU: gone, or detaching */
			error = ENXIO;
			break;
		}
		/* XNU: a stop for sleep / shutdown / detach is time-bounded */
		if (sc->xnu.stop_deadline_us != 0 &&
		    mps_os_uptime_us() > sc->xnu.stop_deadline_us) {
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "IOC not READY in "
			    "time, giving up\n");
			error = ETIMEDOUT;
			break;
		}
		reg = mps_regread(sc, MPI2_DOORBELL_OFFSET);
		mps_dprint(sc, MPS_INIT, "  Doorbell= 0x%x\n", reg);

		/*
		 * Ensure the IOC is ready to talk.  If it's not, try
		 * resetting it.
		 */
		if ((reg & MPI2_DOORBELL_USED) && sc->xnu.stop_fast) {
			error = ETIMEDOUT;	/* XNU: no diag reset at shutdown */
			break;
		}
		if (reg & MPI2_DOORBELL_USED) {
			mps_dprint(sc, MPS_INIT, "  Not ready, sending diag "
			    "reset\n");
			mps_diag_reset(sc, sleep_flags);
			DELAY(50000);
			continue;
		}

		/* Is the adapter owned by another peer? */
		if ((reg & MPI2_DOORBELL_WHO_INIT_MASK) ==
		    (MPI2_WHOINIT_PCI_PEER << MPI2_DOORBELL_WHO_INIT_SHIFT)) {
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "IOC is under the "
			    "control of another peer host, aborting "
			    "initialization.\n");
			error = ENXIO;
			break;
		}
		
		state = reg & MPI2_IOC_STATE_MASK;
		if (state == MPI2_IOC_STATE_READY) {
			/* Ready to go! */
			error = 0;
			break;
		} else if (state == MPI2_IOC_STATE_FAULT) {
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "IOC in fault "
			    "state 0x%x, resetting\n",
			    state & MPI2_DOORBELL_FAULT_CODE_MASK);
			if (sc->xnu.stop_fast) {	/* XNU: shutdown */
				error = ETIMEDOUT;
				break;
			}
			mps_diag_reset(sc, sleep_flags);
		} else if (state == MPI2_IOC_STATE_OPERATIONAL) {
			/* Need to take ownership */
			if (mps_message_unit_reset(sc, sleep_flags) != 0 &&
			    sc->xnu.stop_fast) {
				error = ETIMEDOUT;	/* XNU: MUR once */
				break;
			}
		} else if (state == MPI2_IOC_STATE_RESET) {
			/* Wait a bit, IOC might be in transition */
			mps_dprint(sc, MPS_INIT|MPS_FAULT,
			    "IOC in unexpected reset state\n");
		} else {
			mps_dprint(sc, MPS_INIT|MPS_FAULT,
			    "IOC in unknown state 0x%x\n", state);
			error = EINVAL;
			break;
		}

		/* Wait 50ms for things to settle down. */
		DELAY(50000);
	}

	if (error)
		mps_dprint(sc, MPS_INIT|MPS_FAULT,
		    "Cannot transition IOC to ready\n");
	mps_dprint(sc, MPS_INIT, "%s exit\n", __func__);

	return (error);
}

static int
mps_transition_operational(struct mps_softc *sc)
{
	uint32_t reg, state;
	int error;

	MPS_FUNCTRACE(sc);

	error = 0;
	reg = mps_regread(sc, MPI2_DOORBELL_OFFSET);
	mps_dprint(sc, MPS_INIT, "%s entered, Doorbell= 0x%x\n", __func__, reg);

	state = reg & MPI2_IOC_STATE_MASK;
	if (state != MPI2_IOC_STATE_READY) {
		mps_dprint(sc, MPS_INIT, "IOC not ready\n");
		if ((error = mps_transition_ready(sc)) != 0) {
			mps_dprint(sc, MPS_INIT|MPS_FAULT, 
			    "failed to transition ready, exit\n");
			return (error);
		}
	}

	error = mps_send_iocinit(sc);
	mps_dprint(sc, MPS_INIT, "%s exit\n", __func__);

	return (error);
}

static void
mps_resize_queues(struct mps_softc *sc)
{
	u_int reqcr, prireqcr, maxio, sges_per_frame;

	/*
	 * Size the queues. Since the reply queues always need one free
	 * entry, we'll deduct one reply message here.  The LSI documents
	 * suggest instead to add a count to the request queue, but I think
	 * that it's better to deduct from reply queue.
	 */
	prireqcr = MAX(1, sc->max_prireqframes);
	prireqcr = MIN(prireqcr, sc->facts->HighPriorityCredit);

	reqcr = MAX(2, sc->max_reqframes);
	reqcr = MIN(reqcr, sc->facts->RequestCredit);
	/*
	 * XNU: every request may complete at once, so the reply descriptor
	 * post queue (num_replies + 1 entries below) must hold one descriptor
	 * per request frame: at most MaxReplyDescriptorPostQueueDepth - 1
	 * frames, and num_replies at least num_reqs. FreeBSD's defaults
	 * (2048 + 128 frames, 2048 + 32 replies) can post more completions
	 * than its 2080-entry queue holds.
	 */
	if (sc->facts->MaxReplyDescriptorPostQueueDepth > prireqcr + 3)
		reqcr = MIN(reqcr,
		    sc->facts->MaxReplyDescriptorPostQueueDepth - prireqcr - 1);

	sc->num_reqs = prireqcr + reqcr;
	sc->num_prireqs = prireqcr;
	sc->num_replies = MIN(MAX(sc->max_replyframes, sc->num_reqs) +
	    sc->max_evtframes, sc->facts->MaxReplyDescriptorPostQueueDepth) - 1;

	/* Store the request frame size in bytes rather than as 32bit words */
	sc->reqframesz = sc->facts->IOCRequestFrameSize * 4;

	/*
	 * Max IO Size is Page Size * the following:
	 * ((SGEs per frame - 1 for chain element) * Max Chain Depth)
	 * + 1 for no chain needed in last frame
	 *
	 * If user suggests a Max IO size to use, use the smaller of the
	 * user's value and the calculated value as long as the user's
	 * value is larger than 0. The user's value is in pages.
	 */
	sges_per_frame = sc->reqframesz / sizeof(MPI2_SGE_SIMPLE64) - 1;
	maxio = (sges_per_frame * sc->facts->MaxChainDepth + 1) * PAGE_SIZE;

	/*
	 * If I/O size limitation requested, then use it and pass up to CAM.
	 * If not, use maxphys as an optimization hint, but report HW limit.
	 */
	if (sc->max_io_pages > 0) {
		maxio = min(maxio, sc->max_io_pages * PAGE_SIZE);
		sc->maxio = maxio;
	} else {
		sc->maxio = maxio;
		maxio = min(maxio, maxphys);
	}

	sc->num_chains = (maxio / PAGE_SIZE + sges_per_frame - 2) /
	    sges_per_frame * reqcr;
	if (sc->max_chains > 0 && sc->max_chains < sc->num_chains)
		sc->num_chains = sc->max_chains;
	if (sc->num_chains == 0)	/* XNU: MaxChainDepth 0: no 0-byte DMA */
		sc->num_chains = 1;

	/*
	 * Figure out the number of MSIx-based queues.  If the firmware or
	 * user has done something crazy and not allowed enough credit for
	 * the queues to be useful then don't enable multi-queue.
	 */
	if (sc->facts->MaxMSIxVectors < 2)
		sc->msi_msgs = 1;

	if (sc->msi_msgs > 1) {
		sc->msi_msgs = MIN(sc->msi_msgs, mp_ncpus);
		sc->msi_msgs = MIN(sc->msi_msgs, sc->facts->MaxMSIxVectors);
		if (sc->num_reqs / sc->msi_msgs < 2)
			sc->msi_msgs = 1;
	}

	mps_dprint(sc, MPS_INIT, "Sized queues to q=%d reqs=%d replies=%d\n",
	    sc->msi_msgs, sc->num_reqs, sc->num_replies);
	mps_dprint(sc, MPS_INIT, "RequestCredit %d HighPriorityCredit %d "
	    "ReplyPostQueueDepth %d: %d chain frames\n",
	    sc->facts->RequestCredit, sc->facts->HighPriorityCredit,
	    sc->facts->MaxReplyDescriptorPostQueueDepth, sc->num_chains);
}

/*
 * This is called during attach and when re-initializing due to a Diag Reset.
 * IOC Facts is used to allocate many of the structures needed by the driver.
 * If called from attach, de-allocation is not required because the driver has
 * not allocated any structures yet, but if called from a Diag Reset, previously
 * allocated structures based on IOC Facts will need to be freed and re-
 * allocated bases on the latest IOC Facts.
 */
static int
mps_iocfacts_allocate(struct mps_softc *sc, uint8_t attaching)
{
	int error;
	Mpi2IOCFactsReply_t saved_facts;
	uint8_t saved_mode, reallocating;

	mps_dprint(sc, MPS_INIT|MPS_TRACE, "%s entered\n", __func__);

	/* Save old IOC Facts and then only reallocate if Facts have changed */
	if (!attaching) {
		bcopy(sc->facts, &saved_facts, sizeof(MPI2_IOC_FACTS_REPLY));
	}

	/*
	 * Get IOC Facts.  In all cases throughout this function, panic if doing
	 * a re-initialization and only return the error if attaching so the OS
	 * can handle it.
	 */
	if ((error = mps_get_iocfacts(sc, sc->facts)) != 0) {
		if (attaching) {
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "Failed to get "
			    "IOC Facts with error %d, exit\n", error);
			return (error);
		} else {
			/* XNU: no panic, mps_reinit() retries */
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "%s failed to get "
			    "IOC Facts with error %d\n", __func__, error);
			return (error);
		}
	}

	/*
	 * XNU: the SCSI IO SGL math ((32 - 24) * 4 bytes in the request
	 * frame, mpssas_action_scsiio) assumes request frames of at least 128
	 * bytes; refuse smaller ones instead of overrunning the frame.
	 */
	if (sc->facts->IOCRequestFrameSize < 32) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "IOCRequestFrameSize %u "
		    "dwords too small (need 32)\n",
		    sc->facts->IOCRequestFrameSize);
		return (EINVAL);
	}

	MPS_DPRINT_PAGE(sc, MPS_XINFO, iocfacts, sc->facts);

	snprintf(sc->fw_version, sizeof(sc->fw_version), 
	    "%02d.%02d.%02d.%02d", 
	    sc->facts->FWVersion.Struct.Major,
	    sc->facts->FWVersion.Struct.Minor,
	    sc->facts->FWVersion.Struct.Unit,
	    sc->facts->FWVersion.Struct.Dev);

	snprintf(sc->msg_version, sizeof(sc->msg_version), "%d.%d",
	    (sc->facts->MsgVersion & MPI2_IOCFACTS_MSGVERSION_MAJOR_MASK) >>
	    MPI2_IOCFACTS_MSGVERSION_MAJOR_SHIFT, 
	    (sc->facts->MsgVersion & MPI2_IOCFACTS_MSGVERSION_MINOR_MASK) >>
	    MPI2_IOCFACTS_MSGVERSION_MINOR_SHIFT);

	mps_dprint(sc, MPS_INFO, "Firmware: %s, Driver: %s\n", sc->fw_version,
	    MPS_DRIVER_VERSION);
	mps_dprint(sc, MPS_INFO, "IOCCapabilities: %b\n",
	     sc->facts->IOCCapabilities,
	    "\20" "\3ScsiTaskFull" "\4DiagTrace" "\5SnapBuf" "\6ExtBuf"
	    "\7EEDP" "\10BiDirTarg" "\11Multicast" "\14TransRetry" "\15IR"
	    "\16EventReplay" "\17RaidAccel" "\20MSIXIndex" "\21HostDisc");

	/*
	 * If the chip doesn't support event replay then a hard reset will be
	 * required to trigger a full discovery.  Do the reset here then
	 * retransition to Ready.  A hard reset might have already been done,
	 * but it doesn't hurt to do it again.  Only do this if attaching, not
	 * for a Diag Reset.
	 */
	if (attaching && ((sc->facts->IOCCapabilities &
	    MPI2_IOCFACTS_CAPABILITY_EVENT_REPLAY) == 0)) {
		mps_dprint(sc, MPS_INIT, "No event replay, reseting\n");
		mps_diag_reset(sc, NO_SLEEP);
		if ((error = mps_transition_ready(sc)) != 0) {
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "Failed to "
			    "transition to ready with error %d, exit\n",
			    error);
			return (error);
		}
	}

	/*
	 * Set flag if IR Firmware is loaded.  If the RAID Capability has
	 * changed from the previous IOC Facts, log a warning, but only if
	 * checking this after a Diag Reset and not during attach.
	 */
	saved_mode = sc->ir_firmware;
	if (sc->facts->IOCCapabilities &
	    MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID)
		sc->ir_firmware = 1;
	if (!attaching) {
		if (sc->ir_firmware != saved_mode) {
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "new IR/IT mode "
			    "in IOC Facts does not match previous mode\n");
		}
	}

	/* Only deallocate and reallocate if relevant IOC Facts have changed */
	reallocating = FALSE;
	sc->mps_flags &= ~MPS_FLAGS_REALLOCATED;

	if ((!attaching) &&
	    ((saved_facts.MsgVersion != sc->facts->MsgVersion) ||
	    (saved_facts.HeaderVersion != sc->facts->HeaderVersion) ||
	    (saved_facts.MaxChainDepth != sc->facts->MaxChainDepth) ||
	    (saved_facts.RequestCredit != sc->facts->RequestCredit) ||
	    (saved_facts.ProductID != sc->facts->ProductID) ||
	    (saved_facts.IOCCapabilities != sc->facts->IOCCapabilities) ||
	    (saved_facts.IOCRequestFrameSize !=
	    sc->facts->IOCRequestFrameSize) ||
	    (saved_facts.MaxTargets != sc->facts->MaxTargets) ||
	    (saved_facts.MaxSasExpanders != sc->facts->MaxSasExpanders) ||
	    (saved_facts.MaxEnclosures != sc->facts->MaxEnclosures) ||
	    (saved_facts.HighPriorityCredit != sc->facts->HighPriorityCredit) ||
	    (saved_facts.MaxReplyDescriptorPostQueueDepth !=
	    sc->facts->MaxReplyDescriptorPostQueueDepth) ||
	    (saved_facts.ReplyFrameSize != sc->facts->ReplyFrameSize) ||
	    (saved_facts.MaxVolumes != sc->facts->MaxVolumes) ||
	    (saved_facts.MaxPersistentEntries !=
	    sc->facts->MaxPersistentEntries))) {
		reallocating = TRUE;

		/* Record that we reallocated everything */
		sc->mps_flags |= MPS_FLAGS_REALLOCATED;
	}

	/*
	 * Some things should be done if attaching or re-allocating after a Diag
	 * Reset, but are not needed after a Diag Reset if the FW has not
	 * changed.
	 */
	if (attaching || reallocating) {
		/*
		 * Check if controller supports FW diag buffers and set flag to
		 * enable each type.
		 */
		if (sc->facts->IOCCapabilities &
		    MPI2_IOCFACTS_CAPABILITY_DIAG_TRACE_BUFFER)
			sc->fw_diag_buffer_list[MPI2_DIAG_BUF_TYPE_TRACE].
			    enabled = TRUE;
		if (sc->facts->IOCCapabilities &
		    MPI2_IOCFACTS_CAPABILITY_SNAPSHOT_BUFFER)
			sc->fw_diag_buffer_list[MPI2_DIAG_BUF_TYPE_SNAPSHOT].
			    enabled = TRUE;
		if (sc->facts->IOCCapabilities &
		    MPI2_IOCFACTS_CAPABILITY_EXTENDED_BUFFER)
			sc->fw_diag_buffer_list[MPI2_DIAG_BUF_TYPE_EXTENDED].
			    enabled = TRUE;

		/*
		 * Set flag if EEDP is supported and if TLR is supported.
		 */
		if (sc->facts->IOCCapabilities & MPI2_IOCFACTS_CAPABILITY_EEDP)
			sc->eedp_enabled = TRUE;
		if (sc->facts->IOCCapabilities & MPI2_IOCFACTS_CAPABILITY_TLR)
			sc->control_TLR = TRUE;

		mps_resize_queues(sc);

		/*
		 * Initialize all Tail Queues
		 */
		TAILQ_INIT(&sc->req_list);
		TAILQ_INIT(&sc->high_priority_req_list);
		TAILQ_INIT(&sc->chain_list);
		TAILQ_INIT(&sc->tm_list);
	}

	/*
	 * If doing a Diag Reset and the FW is significantly different
	 * (reallocating will be set above in IOC Facts comparison), then all
	 * buffers based on the IOC Facts will need to be freed before they are
	 * reallocated.
	 */
	if (reallocating) {
		mps_iocfacts_free(sc);
		mpssas_realloc_targets(sc, saved_facts.MaxTargets +
		    saved_facts.MaxVolumes);
	}

	/*
	 * Any deallocation has been completed.  Now start reallocating
	 * if needed.  Will only need to reallocate if attaching or if the new
	 * IOC Facts are different from the previous IOC Facts after a Diag
	 * Reset. Targets have already been allocated above if needed.
	 */
	error = 0;
	while (attaching || reallocating) {
		if ((error = mps_alloc_hw_queues(sc)) != 0)
			break;
		if ((error = mps_alloc_replies(sc)) != 0)
			break;
		if ((error = mps_alloc_requests(sc)) != 0)
			break;
		if ((error = mps_alloc_queues(sc)) != 0)
			break;

		break;
	}
	if (error) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT,
		    "Failed to alloc queues with error %d\n", error);
		/* XNU: mps_attach() frees on every failure */
		return (error);
	}

	/* Always initialize the queues */
	bzero(sc->free_queue, sc->fqdepth * 4);
	mps_init_queues(sc);

	/*
	 * Always get the chip out of the reset state, but only panic if not
	 * attaching.  If attaching and there is an error, that is handled by
	 * the OS.
	 */
	error = mps_transition_operational(sc);
	if (error != 0) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "Failed to "
		    "transition to operational with error %d\n", error);
		/* XNU: mps_attach() frees on every failure */
		return (error);
	}

	/*
	 * Finish the queue initialization.
	 * These are set here instead of in mps_init_queues() because the
	 * IOC resets these values during the state transition in
	 * mps_transition_operational().  The free index is set to 1
	 * because the corresponding index in the IOC is set to 0, and the
	 * IOC treats the queues as full if both are set to the same value.
	 * Hence the reason that the queue can't hold all of the possible
	 * replies.
	 */
	sc->replypostindex = 0;
	mps_regwrite(sc, MPI2_REPLY_FREE_HOST_INDEX_OFFSET, sc->replyfreeindex);
	mps_regwrite(sc, MPI2_REPLY_POST_HOST_INDEX_OFFSET, 0);

	/*
	 * Attach the subsystems so they can prepare their event masks.
	 * XXX Should be dynamic so that IM/IR and user modules can attach
	 */
	error = 0;
	while (attaching) {
		mps_dprint(sc, MPS_INIT, "Attaching subsystems\n");
		if ((error = mps_attach_log(sc)) != 0)
			break;
		if ((error = mps_attach_sas(sc)) != 0)
			break;
		/* XNU: no mps_attach_user() (ioctl interface) */
		break;
	}
	if (error) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "Failed to attach all "
		    "subsystems: error %d\n", error);
		/* XNU: mps_attach() calls mps_free() once for every failure */
		return (error);
	}

	/*
	 * XXX If the number of MSI-X vectors changes during re-init, this
	 * won't see it and adjust.
	 */
	if (attaching && (error = mps_pci_setup_interrupts(sc)) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "Failed to setup "
		    "interrupts\n");
		/* XNU: mps_attach() calls mps_free() once for every failure */
		return (error);
	}

	/*
	 * Set flag if this is a WD controller.  This shouldn't ever change, but
	 * reset it after a Diag Reset, just in case.
	 */
	sc->WD_available = FALSE;
	if (pci_get_device(sc->mps_dev) == MPI2_MFGPAGE_DEVID_SSS6200)
		sc->WD_available = TRUE;

	return (error);
}

/*
 * This is called if memory is being free (during detach for example) and when
 * buffers need to be reallocated due to a Diag Reset.
 */
static void
mps_iocfacts_free(struct mps_softc *sc)
{
	struct mps_command *cm;
	int i;

	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);

	if (sc->free_busaddr != 0)
		bus_dmamap_unload(sc->queues_dmat, sc->queues_map);
	if (sc->free_queue != NULL)
		bus_dmamem_free(sc->queues_dmat, sc->free_queue,
		    sc->queues_map);
	if (sc->queues_dmat != NULL)
		bus_dma_tag_destroy(sc->queues_dmat);

	if (sc->chain_frames != NULL) {
		bus_dmamap_unload(sc->chain_dmat, sc->chain_map);
		bus_dmamem_free(sc->chain_dmat, sc->chain_frames,
		    sc->chain_map);
	}
	if (sc->chain_dmat != NULL)
		bus_dma_tag_destroy(sc->chain_dmat);

	if (sc->sense_busaddr != 0)
		bus_dmamap_unload(sc->sense_dmat, sc->sense_map);
	if (sc->sense_frames != NULL)
		bus_dmamem_free(sc->sense_dmat, sc->sense_frames,
		    sc->sense_map);
	if (sc->sense_dmat != NULL)
		bus_dma_tag_destroy(sc->sense_dmat);

	if (sc->reply_busaddr != 0)
		bus_dmamap_unload(sc->reply_dmat, sc->reply_map);
	if (sc->reply_frames != NULL)
		bus_dmamem_free(sc->reply_dmat, sc->reply_frames,
		    sc->reply_map);
	if (sc->reply_dmat != NULL)
		bus_dma_tag_destroy(sc->reply_dmat);

	if (sc->req_busaddr != 0)
		bus_dmamap_unload(sc->req_dmat, sc->req_map);
	if (sc->req_frames != NULL)
		bus_dmamem_free(sc->req_dmat, sc->req_frames, sc->req_map);
	if (sc->req_dmat != NULL)
		bus_dma_tag_destroy(sc->req_dmat);

	if (sc->chains != NULL)
		free(sc->chains, M_MPT2);
	if (sc->commands != NULL) {
		for (i = 1; i < sc->num_reqs; i++) {
			cm = &sc->commands[i];
			bus_dmamap_destroy(sc->buffer_dmat, cm->cm_dmamap);
		}
		free(sc->commands, M_MPT2);
	}
	if (sc->buffer_dmat != NULL)
		bus_dma_tag_destroy(sc->buffer_dmat);

	mps_pci_free_interrupts(sc);
	free(sc->queues, M_MPT2);
	sc->queues = NULL;

	/*
	 * XNU: forget what was freed, so a second call (a failed allocation
	 * already cleaned up, then mps_free) and a reallocation that fails
	 * half-way never free or use it again.
	 */
	sc->free_busaddr = sc->sense_busaddr = 0;
	sc->reply_busaddr = sc->req_busaddr = 0;
	sc->free_queue = NULL;
	sc->post_queue = NULL;
	sc->chain_frames = NULL;
	sc->sense_frames = NULL;
	sc->reply_frames = NULL;
	sc->req_frames = NULL;
	sc->chains = NULL;
	sc->commands = NULL;
	sc->queues_dmat = sc->chain_dmat = sc->sense_dmat = NULL;
	sc->reply_dmat = sc->req_dmat = sc->buffer_dmat = NULL;
}

/* 
 * The terms diag reset and hard reset are used interchangeably in the MPI
 * docs to mean resetting the controller chip.  In this code diag reset
 * cleans everything up, and the hard reset function just sends the reset
 * sequence to the chip.  This should probably be refactored so that every
 * subsystem gets a reset notification of some sort, and can clean up
 * appropriately.
 */
int
(mps_reinit)(struct mps_softc *sc)
{
	int error;
	struct mpssas_softc *sassc;
	int attempt, resume;		/* XNU */
	uint32_t diag_resets;		/* XNU */

	sassc = sc->sassc;

	MPS_FUNCTRACE(sc);

	mtx_assert(&sc->mps_mtx, MA_OWNED);

	mps_dprint(sc, MPS_INIT|MPS_INFO, "Reinitializing controller\n");
	if (sc->mps_flags & MPS_FLAGS_DIAGRESET) {
		mps_dprint(sc, MPS_INIT, "Reset already in progress\n");
		return 0;
	}
	/*
	 * XNU: mps_xnu_request_reinit.  Requests made while this reset runs
	 * are covered by it (they are dropped while MPS_FLAGS_DIAGRESET is
	 * set), so clearing the flag once, here, covers every exit.
	 */
	sc->xnu.reinit_pending = 0;
	/*
	 * XNU: nothing to reset once gone, disabled or detaching; a
	 * controller found gone is disabled here (everything completed).
	 * An all-ones read still being confirmed (D11, <= 100 ms): wait.
	 */
	for (attempt = 0; sc->xnu.gone_tries != 0 && attempt < 50; attempt++)
		msleep(&sc->xnu.gone_tries, &sc->mps_mtx, 0, "mpsgone", 1);
	resume = sc->xnu.resume_ready;	/* XNU: D12 */
	sc->xnu.resume_ready = 0;
	if (sc->mps_flags & (MPS_FLAGS_XNU_GONE | MPS_FLAGS_SHUTDOWN)) {
		if ((sc->mps_flags & MPS_FLAGS_XNU_GONE) && !sc->xnu.disabled)
			mps_xnu_disable(sc, ENXIO);
		return (ENXIO);
	}
	sc->xnu.reinit_running = 1;	/* XNU: mps_xnu_suspend waits for it */
	sc->xnu.stats.reinits++;
	sc->xnu.reinit_start_us = mps_os_uptime_us();

	/* make sure the completion callbacks can recognize they're getting
	 * a NULL cm_reply due to a reset.
	 */
	sc->mps_flags |= MPS_FLAGS_DIAGRESET;

	/*
	 * Mask interrupts here.
	 */
	mps_dprint(sc, MPS_INIT, "masking interrupts and resetting\n");
	mps_mask_intr(sc);

	/*
	 * XNU: no panic.  Every wait below drops mps_mtx (mps_intr_locked and
	 * new IO see MPS_FLAGS_DIAGRESET meanwhile).  A failure, e.g. a fault
	 * during the sequence, restarts it with a new diag reset, up to
	 * MPS_XNU_REINIT_TRIES attempts; then the controller is disabled.
	 */
	diag_resets = sc->xnu.stats.diag_resets;
	for (attempt = 1; ; attempt++) {
		/*
		 * XNU: on resume, FreeBSD's attach sequence first (D12):
		 * mps_transition_ready leaves a READY IOC alone (our suspend
		 * stopped it with a MUR, and the dock kept it powered), MURs an
		 * OPERATIONAL one and diag-resets only a FAULT or unknown state;
		 * then IOCFacts, IOCInit, port enable.  No PHY reset of every
		 * disk on every wake.  The diag reset stays the fallback.
		 */
		if (attempt == 1 && resume)
			error = mps_transition_ready(sc);
		else
			error = mps_diag_reset(sc, CAN_SLEEP);
		if (error != 0)
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "%s hard reset "
			    "failed with error %d\n", __func__, error);

		/* Restore the PCI state, including the MSI-X registers */
		mps_pci_restore(sc);	/* XNU: after every diag reset (D19) */

		if (attempt == 1) {	/* XNU */
			/* Give the I/O subsystem special priority to get itself prepared */
			mpssas_handle_reinit(sc);
		}

		/*
		 * Get IOC Facts and allocate all structures based on this information.
		 * The attach function will also call mps_iocfacts_allocate at startup.
		 * If relevant values have changed in IOC Facts, this function will free
		 * all of the memory based on IOC Facts and reallocate that memory.
		 */
		if (error == 0 &&
		    (error = mps_iocfacts_allocate(sc, FALSE)) != 0)
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "%s IOC Facts based "
			    "allocation failed with error %d\n", __func__,
			    error);
		if (error == 0 || attempt >= MPS_XNU_REINIT_TRIES ||
		    MPS_XNU_ABANDON(sc))
			break;
		sc->xnu.stats.reinit_failures++;
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "reinit attempt %d of %d "
		    "failed, retrying\n", attempt, MPS_XNU_REINIT_TRIES);
	}
	if (error != 0) {
		sc->xnu.stats.reinit_failures++;
		sc->xnu.reinit_running = 0;
		/*
		 * XNU: abandoned because the system goes to sleep: not a
		 * failure of the controller.  The suspend owns
		 * MPS_FLAGS_DIAGRESET now, the resume reinits again, and the
		 * discovery hold taken by mpssas_handle_reinit() stays for
		 * that reinit (reinit_owed).
		 */
		if (sc->xnu.suspending &&
		    (sc->mps_flags & MPS_FLAGS_XNU_GONE) == 0) {
			mps_dprint(sc, MPS_INIT|MPS_INFO, "reinit abandoned "
			    "for system sleep; the resume reinitializes\n");
			if (sassc != NULL && (sassc->flags & MPSSAS_IN_STARTUP))
				sc->xnu.reinit_owed = 1;
			wakeup(&sc->xnu.reinit_running);
			return (error);
		}
		sc->mps_flags &= ~MPS_FLAGS_DIAGRESET;
		mps_xnu_disable(sc, error);
		wakeup(&sc->xnu.reinit_running);
		return (error);
	}

	/*
	 * XNU: no mps_mapping.c and no static config pages (IOC Page8):
	 * mps_mapping_exit(), mps_base_static_config_pages() and
	 * mps_mapping_initialize() are gone.  Interrupts are masked, so
	 * unmask them before the event registration.
	 */
	mps_unmask_intr(sc);
	sc->mps_flags &= ~MPS_FLAGS_DIAGRESET;

	/*
	 * Restart will reload the event masks clobbered by the reset, and
	 * then enable the port.
	 */
	mps_reregister_events(sc);

	/* the end of discovery will release the simq, so we're done. */
	mps_dprint(sc, MPS_INIT|MPS_XINFO, "Finished sc %p post %u free %u\n", 
	    sc, sc->replypostindex, sc->replyfreeindex);

	mpssas_release_simq_reinit(sassc);
	if (resume)			/* XNU: which way the wake went (D12) */
		mps_xnu_resume_done(sc, sc->xnu.stats.diag_resets != diag_resets);
	mps_xnu_reinit_done(sc);	/* XNU: statistics */
	sc->xnu.reinit_running = 0;
	wakeup(&sc->xnu.reinit_running);
	mps_dprint(sc, MPS_INIT, "%s exit\n", __func__);

	return 0;
}

/*
 * XNU: mps_reinit() sleeps, so callouts (mps_periodic) queue this task
 * instead of calling it.
 */
void
mps_reinit_task(void *arg, int pending)
{
	struct mps_softc *sc = arg;

	mps_lock(sc);
	/* XNU: a reinit that ran meanwhile (mps_wait_command) covered it */
	if (sc->xnu.reinit_pending)
		(mps_reinit)(sc);
	mps_unlock(sc);
}

/*
 * XNU: system sleep and shutdown.  FreeBSD's mps_pci_suspend/resume return
 * EINVAL; this follows Linux mpt3sas in shape (scsih_suspend: block
 * requests, stop the IOC; scsih_resume: reset, unblock).  Linux resumes
 * with a SOFT_RESET (MUR to READY first, diag reset as the fallback); we
 * run FreeBSD's attach sequence (mps_transition_ready, IOCFacts, IOCInit,
 * port enable) with mps_reinit's diag reset as the fallback (D12; the
 * 498 ms measured for a wake before was the reinit alone, not the cost of
 * the UNIT ATTENTIONs and re-IDENTIFYs a diag reset causes).  Called with
 * mps_mtx held; both sleep (the lock is dropped meanwhile).
 *
 * Suspend: new SCSI IO is requeued (held, SIM queue frozen) and OS task
 * management is refused (CAM_BUSY); outstanding commands get up to
 * `drain_ms' to complete, a pending reinit and queued events to finish.
 * Then the periodic watchdog stops, interrupts are masked, the IOC is
 * stopped with a message unit reset (no DMA after it) and what it still
 * owned is completed as after a diag reset (CAM_SCSI_BUS_RESET: the OS
 * retries and the retry is held).  MPS_FLAGS_DIAGRESET stays set and
 * MPS_FLAGS_XNU_SUSPENDED keeps every register access away from the
 * (possibly powered off) device until mps_xnu_resume().
 */
static int
mps_xnu_suspend_busy(struct mps_softc *sc)
{
	struct mpssas_softc *sassc = sc->sassc;

	if (sc->mps_flags & (MPS_FLAGS_XNU_GONE | MPS_FLAGS_SHUTDOWN))
		return (0);
	return (sc->io_cmds_active > 0 || sc->xnu.reinit_pending ||
	    (sc->mps_flags & MPS_FLAGS_DIAGRESET) != 0 ||
	    (sassc != NULL && !TAILQ_EMPTY(&sassc->ev_queue)));
}

int
mps_xnu_suspend(struct mps_softc *sc, u_int drain_ms, int shutdown)
{
	struct mpssas_softc *sassc = sc->sassc;
	int error = 0, start;

	mtx_assert(&sc->mps_mtx, MA_OWNED);
	if (sc->xnu.suspending || (sc->mps_flags & MPS_FLAGS_SHUTDOWN))
		return (0);
	sc->xnu.suspending = 1;
	/*
	 * Shutdown is not sleep: the 30 s halt watchdog runs, nothing will
	 * resume, so new IO fails at once (mpssas_action_scsiio) instead of
	 * being held, the drain is short, the IOC gets one message unit reset
	 * with a short wait and no diag reset.
	 */
	if (shutdown) {
		sc->xnu.shutdown = 1;
		if (drain_ms > MPS_XNU_SHUTDOWN_DRAIN_MS)
			drain_ms = MPS_XNU_SHUTDOWN_DRAIN_MS;
	}
	sc->xnu.stats.suspends++;
	mps_dprint(sc, MPS_INIT|MPS_INFO, "%s: %d commands outstanding\n",
	    shutdown ? "shutting down" : "suspending", sc->io_cmds_active);
	if (!shutdown && sassc != NULL && sassc->sim != NULL &&
	    (sassc->flags & MPSSAS_QUEUE_FROZEN) == 0) {
		xpt_freeze_simq(sassc->sim, 1);
		sassc->flags |= MPSSAS_QUEUE_FROZEN;
	}

	start = ticks;
	while (mps_xnu_suspend_busy(sc) &&
	    (u_int)(ticks - start) < drain_ms * (hz / 1000)) {
		if ((sc->mps_flags & MPS_FLAGS_DIAGRESET) == 0)
			mps_intr_locked(sc);
		msleep(&sc->xnu.suspending, &sc->mps_mtx, 0, "mpssusp",
		    hz / 100);
	}
	if (sc->io_cmds_active > 0 || sc->xnu.reinit_pending ||
	    (sc->mps_flags & MPS_FLAGS_DIAGRESET))
		mps_dprint(sc, MPS_INIT|MPS_RECOVERY|MPS_INFO, "suspend: %d "
		    "commands still outstanding after %u ms%s, stopping the "
		    "IOC\n", sc->io_cmds_active, drain_ms,
		    sc->xnu.reinit_pending ? " (reinit pending)" : "");
	sc->xnu.stats.suspend_drain_left += sc->io_cmds_active;

	/*
	 * A reinit still running gives up at its next wait (MPS_XNU_ABANDON:
	 * stop_reinit) without disabling the controller; wait for it, so that
	 * no two threads drive the doorbell.  If it does not stop in time it
	 * gives up once MPS_FLAGS_XNU_SUSPENDED is set.
	 */
	sc->xnu.stop_reinit = 1;
	start = ticks;
	while (sc->xnu.reinit_running &&
	    (u_int)(ticks - start) < MPS_XNU_REINIT_STOP_MS * (hz / 1000))
		msleep(&sc->xnu.reinit_running, &sc->mps_mtx, 0, "mpssrei",
		    hz / 100);
	sc->xnu.stop_reinit = 0;
	if (sc->xnu.reinit_running)
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "suspend: the reinit did "
		    "not stop in %u ms\n", MPS_XNU_REINIT_STOP_MS);

	callout_stop(&sc->periodic);
	/* A reinit still running gives up at its next wait (MPS_XNU_ABANDON) */
	sc->mps_flags |= MPS_FLAGS_DIAGRESET;
	if ((sc->mps_flags & MPS_FLAGS_XNU_GONE) == 0) {
		mps_mask_intr(sc);
		sc->xnu.stop_fast = shutdown;
		sc->xnu.stop_deadline_us = mps_os_uptime_us() + (uint64_t)1000 *
		    (shutdown ? MPS_XNU_SHUTDOWN_STOP_MS : MPS_XNU_SLEEP_STOP_MS);
		error = mps_transition_ready(sc);
		sc->xnu.stop_fast = 0;
		sc->xnu.stop_deadline_us = 0;
		if (error != 0 && (sc->mps_flags & MPS_FLAGS_XNU_GONE) == 0) {
			/*
			 * The IOC did not stop and may still DMA into the
			 * buffers about to be returned: bus mastering off
			 * before anything completes.
			 */
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "suspend: the IOC "
			    "did not stop (error %d): bus mastering off\n",
			    error);
			mps_os_controller_dead(sc->mps_dev);
		}
	}
	if (shutdown)
		mpssas_xnu_fail_all(sc);	/* held IO too: nothing resumes */
	else
		mpssas_xnu_suspend(sc);
	sc->mps_flags |= MPS_FLAGS_XNU_SUSPENDED;
	sc->xnu.suspended_logged = 0;
	mps_dprint(sc, MPS_INIT|MPS_INFO, "suspended: IOC stopped\n");
	return (error);
}

/*
 * Resume: registers usable again; the reinit (diag reset, IOCFacts,
 * IOCInit, events, port enable = rediscovery, mps_reinit) runs on the
 * reinit taskqueue.  IO stays held until the rediscovery releases the SIM
 * queue; a controller that went away meanwhile is disabled there.
 */
int
mps_xnu_resume(struct mps_softc *sc)
{
	uint32_t db;
	int i;

	mtx_assert(&sc->mps_mtx, MA_OWNED);
	if (!sc->xnu.suspending || sc->xnu.shutdown)
		return (sc->xnu.shutdown ? ENXIO : 0);
	sc->mps_flags &= ~(MPS_FLAGS_XNU_SUSPENDED | MPS_FLAGS_DIAGRESET);
	sc->xnu.suspending = 0;
	sc->xnu.stats.resumes++;
	mps_dprint(sc, MPS_INIT|MPS_INFO, "resuming: reinitializing the "
	    "controller\n");
	if (sc->mps_flags & MPS_FLAGS_SHUTDOWN)
		return (ENXIO);
	/*
	 * The Thunderbolt tunnel may not pass MMIO yet when the PCI device is
	 * powered on: an all-ones doorbell is tolerated for a while (raw
	 * reads, no mps_xnu_gone) before mps_periodic decides.
	 */
	for (i = 0; i < MPS_XNU_RESUME_TRIES; i++) {
		db = bus_space_read_4(sc->mps_btag, sc->mps_bhandle,
		    MPI2_DOORBELL_OFFSET);
		if (db != 0xffffffff)
			break;
		if (i == 0)
			mps_dprint(sc, MPS_INIT|MPS_INFO, "resume: registers "
			    "read all-ones, waiting for the link\n");
		msleep(&sc->xnu.resume_us, &sc->mps_mtx, 0, "mpsres",
		    hz / 10);
	}
	mps_periodic(sc);
	sc->xnu.resume_ready = 1;	/* FreeBSD's attach sequence first (D12) */
	mps_xnu_request_reinit(sc, "resume");
	return (0);
}

/* Wait for the chip to ACK a word that we've put into its FIFO 
 * Wait for <timeout> seconds. In single loop wait for busy loop
 * for 500 microseconds.
 * Total is [ 0.5 * (2000 * <timeout>) ] in miliseconds.
 * */
static int
mps_wait_db_ack(struct mps_softc *sc, int timeout, int sleep_flag)
{

	u32 cntdn, count;
	u32 int_status;
	u32 doorbell;

	count = 0;
	cntdn = (sleep_flag == CAN_SLEEP) ? 1000*timeout : 2000*timeout;
	do {
		int_status = mps_regread(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET);
		if (MPS_XNU_ABANDON(sc))	/* XNU: gone, or detaching */
			goto out;
		if (!(int_status & MPI2_HIS_SYS2IOC_DB_STATUS)) {
			mps_dprint(sc, MPS_TRACE, 
			"%s: successful count(%d), timeout(%d)\n",
			__func__, count, timeout);
		return 0;
		} else if (int_status & MPI2_HIS_IOC2SYS_DB_STATUS) {
			doorbell = mps_regread(sc, MPI2_DOORBELL_OFFSET);
			if ((doorbell & MPI2_IOC_STATE_MASK) ==
				MPI2_IOC_STATE_FAULT) {
				mps_dprint(sc, MPS_FAULT, 
					"fault_state(0x%04x)!\n", doorbell);
				return (EFAULT);
			}
		} else if (int_status == 0xFFFFFFFF)
			goto out;

		/* If it can sleep, sleep for 1 milisecond, else busy loop for 
		* 0.5 milisecond */
		if (mtx_owned(&sc->mps_mtx) && sleep_flag == CAN_SLEEP)
			msleep(&sc->msleep_fake_chan, &sc->mps_mtx, 0, 
			"mpsdba", hz/1000);
		else if (sleep_flag == CAN_SLEEP)
			pause("mpsdba", hz/1000);
		else
			DELAY(500);
		count++;
	} while (--cntdn);

	out:
	mps_dprint(sc, MPS_FAULT, "%s: failed due to timeout count(%d), "
		"int_status(%x)!\n", __func__, count, int_status);
	return (ETIMEDOUT);

}

/* Wait for the chip to signal that the next word in its FIFO can be fetched */
static int
mps_wait_db_int(struct mps_softc *sc)
{
	int retry;

	for (retry = 0; retry < MPS_DB_MAX_WAIT; retry++) {
		if (MPS_XNU_ABANDON(sc))	/* XNU: gone, or detaching */
			return (ENXIO);
		if ((mps_regread(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET) &
		    MPI2_HIS_IOC2SYS_DB_STATUS) != 0)
			return (0);
		DELAY(2000);
	}
	return (ETIMEDOUT);
}

/* Step through the synchronous command state machine, i.e. "Doorbell mode" */
static int
mps_request_sync(struct mps_softc *sc, void *req, MPI2_DEFAULT_REPLY *reply,
    int req_sz, int reply_sz, int timeout)
{
	uint32_t *data32;
	uint16_t *data16;
	int i, count, ioc_sz, residual;
	int sleep_flags = CAN_SLEEP;

	if (curthread->td_no_sleeping != 0)
		sleep_flags = NO_SLEEP;

	/* Step 1 */
	mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);

	/* Step 2 */
	if (mps_regread(sc, MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_USED)
		return (EBUSY);

	/* Step 3
	 * Announce that a message is coming through the doorbell.  Messages
	 * are pushed at 32bit words, so round up if needed.
	 */
	count = (req_sz + 3) / 4;
	mps_regwrite(sc, MPI2_DOORBELL_OFFSET,
	    (MPI2_FUNCTION_HANDSHAKE << MPI2_DOORBELL_FUNCTION_SHIFT) |
	    (count << MPI2_DOORBELL_ADD_DWORDS_SHIFT));

	/* Step 4 */
	if (mps_wait_db_int(sc) ||
	    (mps_regread(sc, MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_USED) == 0) {
		mps_dprint(sc, MPS_FAULT, "Doorbell failed to activate\n");
		return (ENXIO);
	}
	mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);
	if (mps_wait_db_ack(sc, 5, sleep_flags) != 0) {
		mps_dprint(sc, MPS_FAULT, "Doorbell handshake failed\n");
		return (ENXIO);
	}

	/* Step 5 */
	/* Clock out the message data synchronously in 32-bit dwords*/
	data32 = (uint32_t *)req;
	for (i = 0; i < count; i++) {
		mps_regwrite(sc, MPI2_DOORBELL_OFFSET, htole32(data32[i]));
		if (mps_wait_db_ack(sc, 5, sleep_flags) != 0) {
			mps_dprint(sc, MPS_FAULT,
			    "Timeout while writing doorbell\n");
			return (ENXIO);
		}
	}

	/* Step 6 */
	/* Clock in the reply in 16-bit words.  The total length of the
	 * message is always in the 4th byte, so clock out the first 2 words
	 * manually, then loop the rest.
	 */
	data16 = (uint16_t *)reply;
	if (mps_wait_db_int(sc) != 0) {
		mps_dprint(sc, MPS_FAULT, "Timeout reading doorbell 0\n");
		return (ENXIO);
	}
	data16[0] =
	    mps_regread(sc, MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_DATA_MASK;
	mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);
	if (mps_wait_db_int(sc) != 0) {
		mps_dprint(sc, MPS_FAULT, "Timeout reading doorbell 1\n");
		return (ENXIO);
	}
	data16[1] =
	    mps_regread(sc, MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_DATA_MASK;
	mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);

	/* Number of 32bit words in the message */
	ioc_sz = reply->MsgLength;

	/*
	 * Figure out how many 16bit words to clock in without overrunning.
	 * The precision loss with dividing reply_sz can safely be
	 * ignored because the messages can only be multiples of 32bits.
	 */
	residual = 0;
	count = MIN((reply_sz / 4), ioc_sz) * 2;
	if (count < ioc_sz * 2) {
		residual = ioc_sz * 2 - count;
		mps_dprint(sc, MPS_ERROR, "Driver error, throwing away %d "
		    "residual message words\n", residual);
	}

	for (i = 2; i < count; i++) {
		if (mps_wait_db_int(sc) != 0) {
			mps_dprint(sc, MPS_FAULT,
			    "Timeout reading doorbell %d\n", i);
			return (ENXIO);
		}
		data16[i] = mps_regread(sc, MPI2_DOORBELL_OFFSET) &
		    MPI2_DOORBELL_DATA_MASK;
		mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);
	}

	/*
	 * Pull out residual words that won't fit into the provided buffer.
	 * This keeps the chip from hanging due to a driver programming
	 * error.
	 */
	while (residual--) {
		if (mps_wait_db_int(sc) != 0) {
			mps_dprint(sc, MPS_FAULT,
			    "Timeout reading doorbell\n");
			return (ENXIO);
		}
		(void)mps_regread(sc, MPI2_DOORBELL_OFFSET);
		mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);
	}

	/* Step 7 */
	if (mps_wait_db_int(sc) != 0) {
		mps_dprint(sc, MPS_FAULT, "Timeout waiting to exit doorbell\n");
		return (ENXIO);
	}
	if (mps_regread(sc, MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_USED)
		mps_dprint(sc, MPS_FAULT, "Warning, doorbell still active\n");
	mps_regwrite(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0x0);

	return (0);
}

static void
mps_enqueue_request(struct mps_softc *sc, struct mps_command *cm)
{
	request_descriptor_t rd;
	MPS_FUNCTRACE(sc);
	mps_dprint(sc, MPS_TRACE, "SMID %u cm %p ccb %p\n",
	    cm->cm_desc.Default.SMID, cm, cm->cm_ccb);

	if (sc->mps_flags & MPS_FLAGS_ATTACH_DONE && !(sc->mps_flags & MPS_FLAGS_SHUTDOWN))
		mtx_assert(&sc->mps_mtx, MA_OWNED);

	if (++sc->io_cmds_active > sc->io_cmds_highwater)
		sc->io_cmds_highwater++;
	rd.u.low = cm->cm_desc.Words.Low;
	rd.u.high = cm->cm_desc.Words.High;
	rd.word = htole64(rd.word);

	KASSERT(cm->cm_state == MPS_CM_STATE_BUSY,
	    ("command not busy, state = %u\n", cm->cm_state));
	cm->cm_state = MPS_CM_STATE_INQUEUE;
	if (__predict_false(sc->xnu.debug_lose != 0) &&
	    mps_xnu_debug_lose(sc, cm))
		return;		/* XNU: debug, a lost command (sas2308ctl) */

	/* TODO-We may need to make below regwrite atomic */
	mps_regwrite(sc, MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET,
	    rd.u.low);
	mps_regwrite(sc, MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET,
	    rd.u.high);
}

/*
 * Just the FACTS, ma'am.
 */
static int
mps_get_iocfacts(struct mps_softc *sc, MPI2_IOC_FACTS_REPLY *facts)
{
	MPI2_DEFAULT_REPLY *reply;
	MPI2_IOC_FACTS_REQUEST request;
	int error, req_sz, reply_sz, retry = 0;

	MPS_FUNCTRACE(sc);
	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);

	req_sz = sizeof(MPI2_IOC_FACTS_REQUEST);
	reply_sz = sizeof(MPI2_IOC_FACTS_REPLY);
	reply = (MPI2_DEFAULT_REPLY *)facts;

	/*
	 * Retry sending the initialization sequence. Sometimes, especially with
	 * older firmware, the initialization process fails. Retrying allows the
	 * error to clear in the firmware.
	 */
	bzero(&request, req_sz);
	request.Function = MPI2_FUNCTION_IOC_FACTS;
	while (retry < 5) {
		error = mps_request_sync(sc, &request, reply, req_sz, reply_sz, 5);
		if (error == 0)
			break;
		mps_dprint(sc, MPS_FAULT, "%s failed retry %d\n", __func__, retry);
		DELAY(1000);
                retry++;
	}

	return (error);
}

static int
mps_send_iocinit(struct mps_softc *sc)
{
	MPI2_IOC_INIT_REQUEST	init;
	MPI2_DEFAULT_REPLY	reply;
	int req_sz, reply_sz, error, retry = 0;
	struct timeval now;
	uint64_t time_in_msec;

	MPS_FUNCTRACE(sc);
	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);

	/* Do a quick sanity check on proper initialization */
	if ((sc->pqdepth == 0) || (sc->fqdepth == 0) || (sc->reqframesz == 0)
	    || (sc->replyframesz == 0)) {
		mps_dprint(sc, MPS_INIT|MPS_ERROR,
		    "Driver not fully initialized for IOCInit\n");
		return (EINVAL);
	}

	req_sz = sizeof(MPI2_IOC_INIT_REQUEST);
	reply_sz = sizeof(MPI2_IOC_INIT_REPLY);
	bzero(&init, req_sz);
	bzero(&reply, reply_sz);

	/*
	 * Fill in the init block.  Note that most addresses are
	 * deliberately in the lower 32bits of memory.  This is a micro-
	 * optimzation for PCI/PCIX, though it's not clear if it helps PCIe.
	 */
	init.Function = MPI2_FUNCTION_IOC_INIT;
	init.WhoInit = MPI2_WHOINIT_HOST_DRIVER;
	init.MsgVersion = htole16(MPI2_VERSION);
	init.HeaderVersion = htole16(MPI2_HEADER_VERSION);
	init.SystemRequestFrameSize = htole16((uint16_t)(sc->reqframesz / 4));
	init.ReplyDescriptorPostQueueDepth = htole16(sc->pqdepth);
	init.ReplyFreeQueueDepth = htole16(sc->fqdepth);
	init.SenseBufferAddressHigh = 0;
	init.SystemReplyAddressHigh = 0;
	init.SystemRequestFrameBaseAddress.High = 0;
	init.SystemRequestFrameBaseAddress.Low = htole32((uint32_t)sc->req_busaddr);
	init.ReplyDescriptorPostQueueAddress.High = 0;
	init.ReplyDescriptorPostQueueAddress.Low = htole32((uint32_t)sc->post_busaddr);
	init.ReplyFreeQueueAddress.High = 0;
	init.ReplyFreeQueueAddress.Low = htole32((uint32_t)sc->free_busaddr);
	getmicrotime(&now);
	time_in_msec = (now.tv_sec * 1000 + now.tv_usec/1000);
	init.TimeStamp.High = htole32((time_in_msec >> 32) & 0xFFFFFFFF);
	init.TimeStamp.Low = htole32(time_in_msec & 0xFFFFFFFF);
	/*
	 * Retry sending the initialization sequence. Sometimes, especially with
	 * older firmware, the initialization process fails. Retrying allows the
	 * error to clear in the firmware.
	 */
	while (retry < 5) {
		error = mps_request_sync(sc, &init, &reply, req_sz, reply_sz, 5);
		if ((reply.IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS)
			error = ENXIO;
                if (error == 0)
                        break;
                mps_dprint(sc, MPS_FAULT, "%s failed retry %d\n", __func__, retry);
		DELAY(1000);
                retry++;
        }

	mps_dprint(sc, MPS_INIT, "IOCInit status= 0x%x\n", reply.IOCStatus);
	mps_dprint(sc, MPS_INIT, "%s exit\n", __func__);
	return (error);
}

void
mps_memaddr_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	bus_addr_t *addr;

	addr = arg;
	*addr = segs[0].ds_addr;
}

static int
mps_alloc_queues(struct mps_softc *sc)
{
	struct mps_queue *q;
	u_int nq, i;

	nq = sc->msi_msgs;
	mps_dprint(sc, MPS_INIT|MPS_XINFO, "Allocating %d I/O queues\n", nq);

	sc->queues = malloc(sizeof(struct mps_queue) * nq, M_MPT2,
	    M_NOWAIT|M_ZERO);
	if (sc->queues == NULL)
		return (ENOMEM);

	for (i = 0; i < nq; i++) {
		q = &sc->queues[i];
		mps_dprint(sc, MPS_INIT, "Configuring queue %d %p\n", i, q);
		q->sc = sc;
		q->qnum = i;
	}

	return (0);
}

static int
mps_alloc_hw_queues(struct mps_softc *sc)
{
	bus_dma_template_t t;
	bus_addr_t queues_busaddr;
	uint8_t *queues;
	int qsize, fqsize, pqsize;

	/*
	 * The reply free queue contains 4 byte entries in multiples of 16 and
	 * aligned on a 16 byte boundary. There must always be an unused entry.
	 * This queue supplies fresh reply frames for the firmware to use.
	 *
	 * The reply descriptor post queue contains 8 byte entries in
	 * multiples of 16 and aligned on a 16 byte boundary.  This queue
	 * contains filled-in reply frames sent from the firmware to the host.
	 *
	 * These two queues are allocated together for simplicity.
	 */
	sc->fqdepth = roundup2(sc->num_replies + 1, 16);
	sc->pqdepth = roundup2(sc->num_replies + 1, 16);
	fqsize= sc->fqdepth * 4;
	pqsize = sc->pqdepth * 8;
	qsize = fqsize + pqsize;

	bus_dma_template_init(&t, sc->mps_parent_dmat);
	BUS_DMA_TEMPLATE_FILL(&t, BD_ALIGNMENT(16), BD_MAXSIZE(qsize),
	    BD_MAXSEGSIZE(qsize), BD_NSEGMENTS(1),
	    BD_LOWADDR(BUS_SPACE_MAXADDR_32BIT));
	if (bus_dma_template_tag(&t, &sc->queues_dmat)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate queues DMA tag\n");
		return (ENOMEM);
        }
        if (bus_dmamem_alloc(sc->queues_dmat, (void **)&queues, BUS_DMA_NOWAIT,
	    &sc->queues_map)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate queues memory\n");
		return (ENOMEM);
        }
        bzero(queues, qsize);
        bus_dmamap_load(sc->queues_dmat, sc->queues_map, queues, qsize,
	    mps_memaddr_cb, &queues_busaddr, 0);

	sc->free_queue = (uint32_t *)queues;
	sc->free_busaddr = queues_busaddr;
	sc->post_queue = (MPI2_REPLY_DESCRIPTORS_UNION *)(queues + fqsize);
	sc->post_busaddr = queues_busaddr + fqsize;
	mps_dprint(sc, MPS_INIT, "free queue busaddr= %#016jx size= %d\n",
	    (uintmax_t)sc->free_busaddr, fqsize);
	mps_dprint(sc, MPS_INIT, "reply queue busaddr= %#016jx size= %d\n",
	    (uintmax_t)sc->post_busaddr, pqsize);

	return (0);
}

static int
mps_alloc_replies(struct mps_softc *sc)
{
	bus_dma_template_t t;
	int rsize, num_replies;

	/* Store the reply frame size in bytes rather than as 32bit words */
	sc->replyframesz = sc->facts->ReplyFrameSize * 4;

	/*
	 * sc->num_replies should be one less than sc->fqdepth.  We need to
	 * allocate space for sc->fqdepth replies, but only sc->num_replies
	 * replies can be used at once.
	 */
	num_replies = max(sc->fqdepth, sc->num_replies);

	rsize = sc->replyframesz * num_replies; 
	bus_dma_template_init(&t, sc->mps_parent_dmat);
	BUS_DMA_TEMPLATE_FILL(&t, BD_ALIGNMENT(4), BD_MAXSIZE(rsize),
	    BD_MAXSEGSIZE(rsize), BD_NSEGMENTS(1),
	    BD_LOWADDR(BUS_SPACE_MAXADDR_32BIT));
	if (bus_dma_template_tag(&t, &sc->reply_dmat)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate replies DMA tag\n");
		return (ENOMEM);
        }
        if (bus_dmamem_alloc(sc->reply_dmat, (void **)&sc->reply_frames,
	    BUS_DMA_NOWAIT, &sc->reply_map)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate replies memory\n");
		return (ENOMEM);
        }
        bzero(sc->reply_frames, rsize);
        bus_dmamap_load(sc->reply_dmat, sc->reply_map, sc->reply_frames, rsize,
	    mps_memaddr_cb, &sc->reply_busaddr, 0);

	mps_dprint(sc, MPS_INIT, "reply frames busaddr= %#016jx size= %d\n",
	    (uintmax_t)sc->reply_busaddr, rsize);

	return (0);
}

static void
mps_load_chains_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct mps_softc *sc = arg;
	struct mps_chain *chain;
	bus_size_t bo;
	int i, o, s;

	if (error != 0)
		return;

	for (i = 0, o = 0, s = 0; s < nsegs; s++) {
		KASSERT(segs[s].ds_addr + segs[s].ds_len - 1 <= BUS_SPACE_MAXADDR_32BIT,
		    ("mps: Bad segment address %#jx len %#jx\n", (uintmax_t)segs[s].ds_addr,
			(uintmax_t)segs[s].ds_len));
		for (bo = 0; bo + sc->reqframesz <= segs[s].ds_len;
		    bo += sc->reqframesz) {
			chain = &sc->chains[i++];
			chain->chain =(MPI2_SGE_IO_UNION *)(sc->chain_frames+o);
			chain->chain_busaddr = segs[s].ds_addr + bo;
			o += sc->reqframesz;
			mps_free_chain(sc, chain);
		}
		if (bo != segs[s].ds_len)
			o += segs[s].ds_len - bo;
	}
	sc->chain_free_lowwater = i;
}

static int
mps_alloc_requests(struct mps_softc *sc)
{
	bus_dma_template_t t;
	struct mps_command *cm;
	int i, rsize, nsegs;

	rsize = sc->reqframesz * sc->num_reqs;
	bus_dma_template_init(&t, sc->mps_parent_dmat);
	BUS_DMA_TEMPLATE_FILL(&t, BD_ALIGNMENT(16), BD_MAXSIZE(rsize),
	    BD_MAXSEGSIZE(rsize), BD_NSEGMENTS(1),
	    BD_LOWADDR(BUS_SPACE_MAXADDR_32BIT));
        if (bus_dma_template_tag(&t, &sc->req_dmat)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate request DMA tag\n");
		return (ENOMEM);
        }
        if (bus_dmamem_alloc(sc->req_dmat, (void **)&sc->req_frames,
	    BUS_DMA_NOWAIT, &sc->req_map)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate request memory\n");
		return (ENOMEM);
        }
        bzero(sc->req_frames, rsize);
        bus_dmamap_load(sc->req_dmat, sc->req_map, sc->req_frames, rsize,
	    mps_memaddr_cb, &sc->req_busaddr, 0);
	mps_dprint(sc, MPS_INIT, "request frames busaddr= %#016jx size= %d\n",
	    (uintmax_t)sc->req_busaddr, rsize);

	sc->chains = malloc(sizeof(struct mps_chain) * sc->num_chains, M_MPT2,
	    M_NOWAIT | M_ZERO);
	if (!sc->chains) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate chain memory\n");
		return (ENOMEM);
	}
	rsize = sc->reqframesz * sc->num_chains;
	bus_dma_template_clone(&t, sc->req_dmat);
	BUS_DMA_TEMPLATE_FILL(&t, BD_MAXSIZE(rsize), BD_MAXSEGSIZE(rsize),
	    BD_NSEGMENTS(howmany(rsize, PAGE_SIZE)),
	    BD_BOUNDARY(BUS_SPACE_MAXSIZE_32BIT+1));
	if (bus_dma_template_tag(&t, &sc->chain_dmat)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate chain DMA tag\n");
		return (ENOMEM);
	}
	if (bus_dmamem_alloc(sc->chain_dmat, (void **)&sc->chain_frames,
	    BUS_DMA_NOWAIT | BUS_DMA_ZERO, &sc->chain_map)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate chain memory\n");
		return (ENOMEM);
	}
	if (bus_dmamap_load(sc->chain_dmat, sc->chain_map, sc->chain_frames,
	    rsize, mps_load_chains_cb, sc, BUS_DMA_NOWAIT)) {
		mps_dprint(sc, MPS_ERROR, "Cannot load chain memory\n");
		bus_dmamem_free(sc->chain_dmat, sc->chain_frames,
		    sc->chain_map);
		sc->chain_frames = NULL;	/* XNU: mps_free frees it otherwise */
		return (ENOMEM);
	}

	rsize = MPS_SENSE_LEN * sc->num_reqs;
	bus_dma_template_clone(&t, sc->req_dmat);
	BUS_DMA_TEMPLATE_FILL(&t, BD_ALIGNMENT(1), BD_MAXSIZE(rsize),
	    BD_MAXSEGSIZE(rsize));
        if (bus_dma_template_tag(&t, &sc->sense_dmat)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate sense DMA tag\n");
		return (ENOMEM);
        }
        if (bus_dmamem_alloc(sc->sense_dmat, (void **)&sc->sense_frames,
	    BUS_DMA_NOWAIT, &sc->sense_map)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate sense memory\n");
		return (ENOMEM);
        }
        bzero(sc->sense_frames, rsize);
        bus_dmamap_load(sc->sense_dmat, sc->sense_map, sc->sense_frames, rsize,
	    mps_memaddr_cb, &sc->sense_busaddr, 0);
	mps_dprint(sc, MPS_INIT, "sense frames busaddr= %#016jx size= %d\n",
	    (uintmax_t)sc->sense_busaddr, rsize);

	nsegs = (sc->maxio / PAGE_SIZE) + 1;
	/*
	 * XNU: mps_push_sge puts 1 SGE in the request frame and
	 * reqframesz / 12 - 1 in each chain frame, so more than that times
	 * IOCFacts MaxChainDepth, plus 1, would exceed the chain depth (the
	 * "+ 1" above can, for a misaligned maximum-size transfer).
	 */
	nsegs = MIN(nsegs, (sc->reqframesz / sizeof(MPI2_SGE_SIMPLE64) - 1) *
	    sc->facts->MaxChainDepth + 1);
	bus_dma_template_init(&t, sc->mps_parent_dmat);
	BUS_DMA_TEMPLATE_FILL(&t, BD_MAXSIZE(BUS_SPACE_MAXSIZE_32BIT),
	    BD_NSEGMENTS(nsegs), BD_MAXSEGSIZE(BUS_SPACE_MAXSIZE_24BIT),
	    BD_FLAGS(BUS_DMA_ALLOCNOW), BD_LOCKFUNC(busdma_lock_mutex),
	    BD_LOCKFUNCARG(&sc->mps_mtx),
	    BD_BOUNDARY(BUS_SPACE_MAXSIZE_32BIT+1));
        if (bus_dma_template_tag(&t, &sc->buffer_dmat)) {
		mps_dprint(sc, MPS_ERROR, "Cannot allocate buffer DMA tag\n");
		return (ENOMEM);
        }

	/*
	 * SMID 0 cannot be used as a free command per the firmware spec.
	 * Just drop that command instead of risking accounting bugs.
	 */
	sc->commands = malloc(sizeof(struct mps_command) * sc->num_reqs,
	    M_MPT2, M_WAITOK | M_ZERO);
	if (sc->commands == NULL) {	/* XNU: IOMalloc can fail */
		mps_dprint(sc, MPS_ERROR, "Cannot allocate commands\n");
		return (ENOMEM);
	}
	for (i = 1; i < sc->num_reqs; i++) {
		cm = &sc->commands[i];
		cm->cm_req = sc->req_frames + i * sc->reqframesz;
		cm->cm_req_busaddr = sc->req_busaddr + i * sc->reqframesz;
		cm->cm_sense = &sc->sense_frames[i];
		cm->cm_sense_busaddr = sc->sense_busaddr + i * MPS_SENSE_LEN;
		cm->cm_desc.Default.SMID = i;
		cm->cm_sc = sc;
		cm->cm_state = MPS_CM_STATE_BUSY;
		TAILQ_INIT(&cm->cm_chain_list);
		callout_init_mtx(&cm->cm_callout, &sc->mps_mtx, 0);

		/* XXX Is a failure here a critical problem? */
		if (bus_dmamap_create(sc->buffer_dmat, 0, &cm->cm_dmamap) == 0)
			if (i <= sc->num_prireqs)
				mps_free_high_priority_command(sc, cm);
			else
				mps_free_command(sc, cm);
		else {
			/*
			 * XNU: no panic: fail the allocation;
			 * mps_iocfacts_free destroys the maps made so far.
			 */
			mps_dprint(sc, MPS_ERROR, "failed to allocate command "
			    "%d\n", i);
			sc->num_reqs = i;
			return (ENOMEM);
		}
	}

	return (0);
}

static int
mps_init_queues(struct mps_softc *sc)
{
	int i;

	memset((uint8_t *)sc->post_queue, 0xff, sc->pqdepth * 8);

	/*
	 * According to the spec, we need to use one less reply than we
	 * have space for on the queue.  So sc->num_replies (the number we
	 * use) should be less than sc->fqdepth (allocated size).
	 */
	if (sc->num_replies >= sc->fqdepth)
		return (EINVAL);

	/*
	 * Initialize all of the free queue entries.
	 */
	for (i = 0; i < sc->fqdepth; i++)
		sc->free_queue[i] = sc->reply_busaddr + (i * sc->replyframesz);
	sc->replyfreeindex = sc->num_replies;

	return (0);
}

/* Get the driver parameter tunables.  Lowest priority are the driver defaults.
 * Next are the global settings, if they exist.  Highest are the per-unit
 * settings, if they exist.
 */
void
mps_get_tunables(struct mps_softc *sc)
{
	char tmpstr[80], mps_debug[80];

	/* XXX default to some debugging for now */
	sc->mps_debug = MPS_INFO|MPS_FAULT|MPS_ERROR;	/* XNU: +error (D22) */
	sc->disable_msix = 0;
	sc->disable_msi = 0;
	sc->max_msix = MPS_MSIX_MAX;
	sc->max_chains = MPS_CHAIN_FRAMES;
	sc->max_io_pages = MPS_MAXIO_PAGES;
	sc->enable_ssu = MPS_SSU_ENABLE_SSD_DISABLE_HDD;
	sc->spinup_wait_time = DEFAULT_SPINUP_WAIT;
	sc->use_phynum = 1;
	sc->max_reqframes = MPS_REQ_FRAMES;
	sc->max_prireqframes = MPS_PRI_REQ_FRAMES;
	sc->max_replyframes = MPS_REPLY_FRAMES;
	sc->max_evtframes = MPS_EVT_REPLY_FRAMES;
	sc->msi_msgs = 1;	/* XNU: one MSI vector (mps_pci.c sets it) */

	/*
	 * Grab the global variables.
	 */
	bzero(mps_debug, 80);
	if (TUNABLE_STR_FETCH("hw.mps.debug_level", mps_debug, 80) != 0)
		mps_parse_debug(sc, mps_debug);
	TUNABLE_INT_FETCH("hw.mps.disable_msix", &sc->disable_msix);
	TUNABLE_INT_FETCH("hw.mps.disable_msi", &sc->disable_msi);
	TUNABLE_INT_FETCH("hw.mps.max_msix", &sc->max_msix);
	TUNABLE_INT_FETCH("hw.mps.max_chains", &sc->max_chains);
	TUNABLE_INT_FETCH("hw.mps.max_io_pages", &sc->max_io_pages);
	TUNABLE_INT_FETCH("hw.mps.enable_ssu", &sc->enable_ssu);
	TUNABLE_INT_FETCH("hw.mps.spinup_wait_time", &sc->spinup_wait_time);
	TUNABLE_INT_FETCH("hw.mps.use_phy_num", &sc->use_phynum);
	TUNABLE_INT_FETCH("hw.mps.max_reqframes", &sc->max_reqframes);
	TUNABLE_INT_FETCH("hw.mps.max_prireqframes", &sc->max_prireqframes);
	TUNABLE_INT_FETCH("hw.mps.max_replyframes", &sc->max_replyframes);
	TUNABLE_INT_FETCH("hw.mps.max_evtframes", &sc->max_evtframes);

	/* Grab the unit-instance variables */
	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.debug_level",
	    device_get_unit(sc->mps_dev));
	bzero(mps_debug, 80);
	if (TUNABLE_STR_FETCH(tmpstr, mps_debug, 80) != 0)
		mps_parse_debug(sc, mps_debug);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.disable_msix",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->disable_msix);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.disable_msi",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->disable_msi);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.max_msix",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->max_msix);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.max_chains",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->max_chains);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.max_io_pages",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->max_io_pages);

	bzero(sc->exclude_ids, sizeof(sc->exclude_ids));
	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.exclude_ids",
	    device_get_unit(sc->mps_dev));
	TUNABLE_STR_FETCH(tmpstr, sc->exclude_ids, sizeof(sc->exclude_ids));

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.enable_ssu",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->enable_ssu);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.spinup_wait_time",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->spinup_wait_time);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.use_phy_num",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->use_phynum);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.max_reqframes",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->max_reqframes);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.max_prireqframes",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->max_prireqframes);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.max_replyframes",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->max_replyframes);

	snprintf(tmpstr, sizeof(tmpstr), "dev.mps.%d.max_evtframes",
	    device_get_unit(sc->mps_dev));
	TUNABLE_INT_FETCH(tmpstr, &sc->max_evtframes);

}

static struct mps_debug_string {
	char	*name;
	int	flag;
} mps_debug_strings[] = {
	{"info", MPS_INFO},
	{"fault", MPS_FAULT},
	{"event", MPS_EVENT},
	{"log", MPS_LOG},
	{"recovery", MPS_RECOVERY},
	{"error", MPS_ERROR},
	{"init", MPS_INIT},
	{"xinfo", MPS_XINFO},
	{"user", MPS_USER},
	{"mapping", MPS_MAPPING},
	{"trace", MPS_TRACE}
};

enum mps_debug_level_combiner {
	COMB_NONE,
	COMB_ADD,
	COMB_SUB
};

void	/* XNU: not static, the kext parses its debug property with it */
mps_parse_debug(struct mps_softc *sc, char *list)
{
	struct mps_debug_string *string;
	enum mps_debug_level_combiner op;
	char *token, *endtoken;
	size_t sz;
	int flags, i;

	if (list == NULL || *list == '\0')
		return;

	if (*list == '+') {
		op = COMB_ADD;
		list++;
	} else if (*list == '-') {
		op = COMB_SUB;
		list++;
	} else
		op = COMB_NONE;
	if (*list == '\0')
		return;

	flags = 0;
	sz = sizeof(mps_debug_strings) / sizeof(mps_debug_strings[0]);
	while ((token = strsep(&list, ":,")) != NULL) {
		/* Handle integer flags */
		flags |= strtol(token, &endtoken, 0);
		if (token != endtoken)
			continue;

		/* Handle text flags */
		for (i = 0; i < sz; i++) {
			string = &mps_debug_strings[i];
			if (strcasecmp(token, string->name) == 0) {
				flags |= string->flag;
				break;
			}
		}
	}

	switch (op) {
	case COMB_NONE:
		sc->mps_debug = flags;
		break;
	case COMB_ADD:
		sc->mps_debug |= flags;
		break;
	case COMB_SUB:
		sc->mps_debug &= (~flags);
		break;
	}

	return;
}

int
mps_attach(struct mps_softc *sc)
{
	int error;

	MPS_FUNCTRACE(sc);
	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);

	mtx_init(&sc->mps_mtx, "MPT2SAS lock", NULL, MTX_DEF);
	callout_init_mtx(&sc->periodic, &sc->mps_mtx, 0);
	callout_init_mtx(&sc->device_check_callout, &sc->mps_mtx, 0);
	callout_init_mtx(&sc->xnu.gone_callout, &sc->mps_mtx, 0);	/* XNU */
	TAILQ_INIT(&sc->event_list);
	timevalclear(&sc->lastfail);

	/* XNU: mps_reinit() runs on this taskqueue, see mps_periodic() */
	TASK_INIT(&sc->mps_reinit_task, 0, mps_reinit_task, sc);
	sc->mps_tq = taskqueue_create("mps_reinit", M_WAITOK | M_ZERO,
	    taskqueue_thread_enqueue, &sc->mps_tq);
	if (sc->mps_tq == NULL || taskqueue_start_threads(&sc->mps_tq, 1,
	    PRIBIO, "%s reinit", device_get_nameunit(sc->mps_dev)) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "cannot start the reinit "
		    "taskqueue\n");
		mps_free(sc);
		return (ENOMEM);
	}

	if ((error = mps_transition_ready(sc)) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "failed to transition "
		    "ready\n");
		mps_free(sc);	/* XNU: here, not in the caller (mps_pci.c) */
		return (error);
	}

	sc->facts = malloc(sizeof(MPI2_IOC_FACTS_REPLY), M_MPT2,
	    M_ZERO|M_NOWAIT);
	if(!sc->facts) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "Cannot allocate memory, "
		    "exit\n");
		mps_free(sc);	/* XNU */
		return (ENOMEM);
	}

	/*
	 * Get IOC Facts and allocate all structures based on this information.
	 * A Diag Reset will also call mps_iocfacts_allocate and re-read the IOC
	 * Facts. If relevant values have changed in IOC Facts, this function
	 * will free all of the memory based on IOC Facts and reallocate that
	 * memory.  If this fails, any allocated memory should already be freed.
	 */
	if ((error = mps_iocfacts_allocate(sc, TRUE)) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "IOC Facts based allocation "
		    "failed with error %d, exit\n", error);
		mps_free(sc);	/* XNU: once, for every failure */
		return (error);
	}

	/* Start the periodic watchdog check on the IOC Doorbell */
	mps_periodic(sc);

	/*
	 * The portenable will kick off discovery events that will drive the
	 * rest of the initialization process.  The CAM/SAS module will
	 * hold up the boot sequence until discovery is complete.
	 */
	sc->mps_ich.ich_func = mps_startup;
	sc->mps_ich.ich_arg = sc;
	if (config_intrhook_establish(&sc->mps_ich) != 0) {
		mps_dprint(sc, MPS_INIT|MPS_ERROR,
		    "Cannot establish MPS config hook\n");
		error = EINVAL;
	}

	/* XNU: no IR shutdown handler, no sysctl tree (kext properties) */

	sc->mps_flags |= MPS_FLAGS_ATTACH_DONE;
	mps_dprint(sc, MPS_INIT, "%s exit error= %d\n", __func__, error);

	return (error);
}

/* Run through any late-start handlers. */
static void
mps_startup(void *arg)
{
	struct mps_softc *sc;

	sc = (struct mps_softc *)arg;
	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);

	mps_lock(sc);
	mps_unmask_intr(sc);

	/* XNU: no device mapping tables (no config pages, mps_mapping.c) */
	mpssas_startup(sc);
	mps_unlock(sc);

	mps_dprint(sc, MPS_INIT, "disestablish config intrhook\n");
	config_intrhook_disestablish(&sc->mps_ich);
	sc->mps_ich.ich_arg = NULL;

	mps_dprint(sc, MPS_INIT, "%s exit\n", __func__);
}

/* Periodic watchdog.  Is called with the driver lock already held. */
static void
mps_periodic(void *arg)
{
	struct mps_softc *sc;
	uint32_t db;

	sc = (struct mps_softc *)arg;
	if (sc->mps_flags & MPS_FLAGS_SHUTDOWN)
		return;
	if (sc->mps_flags & MPS_FLAGS_XNU_GONE)	/* XNU: gone or disabled */
		return;

	db = mps_regread(sc, MPI2_DOORBELL_OFFSET);
	if ((db & MPI2_IOC_STATE_MASK) == MPI2_IOC_STATE_FAULT &&
	    (sc->mps_flags & MPS_FLAGS_DIAGRESET) == 0 &&	/* XNU */
	    !sc->xnu.reinit_pending) {				/* XNU */
		mps_dprint(sc, MPS_FAULT, "IOC Fault 0x%08x, Resetting\n", db);
		sc->xnu.stats.faults++;
		mps_reinit(sc);
	}
	mps_xnu_periodic(sc);	/* XNU: log summaries, statistics */

	callout_reset_sbt(&sc->periodic, MPS_PERIODIC_DELAY * SBT_1S, 0,
	    mps_periodic, sc, C_PREL(1));
}

static void
mps_log_evt_handler(struct mps_softc *sc, uintptr_t data,
    MPI2_EVENT_NOTIFICATION_REPLY *event)
{
	MPI2_EVENT_DATA_LOG_ENTRY_ADDED *entry;

	MPS_DPRINT_EVENT(sc, generic, event);

	switch (event->Event) {
	case MPI2_EVENT_LOG_DATA:
		mps_dprint(sc, MPS_EVENT, "MPI2_EVENT_LOG_DATA:\n");
		if (sc->mps_debug & MPS_EVENT)
			hexdump(event->EventData, event->EventDataLength, NULL, 0);
		break;
	case MPI2_EVENT_LOG_ENTRY_ADDED:
		entry = (MPI2_EVENT_DATA_LOG_ENTRY_ADDED *)event->EventData;
		mps_dprint(sc, MPS_EVENT, "MPI2_EVENT_LOG_ENTRY_ADDED event "
		    "0x%x Sequence %d:\n", entry->LogEntryQualifier,
		     entry->LogSequence);
		break;
	default:
		break;
	}
	return;
}

static int
mps_attach_log(struct mps_softc *sc)
{
	u32 events[MPI2_EVENT_NOTIFY_EVENTMASK_WORDS];

	bzero(events, 16);
	setbit(events, MPI2_EVENT_LOG_DATA);
	setbit(events, MPI2_EVENT_LOG_ENTRY_ADDED);

	/* XNU: a failed registration fails the attach */
	return (mps_register_events(sc, events, mps_log_evt_handler, NULL,
	    &sc->mps_log_eh));
}

static int
mps_detach_log(struct mps_softc *sc)
{

	if (sc->mps_log_eh != NULL)
		mps_deregister_events(sc, sc->mps_log_eh);
	return (0);
}

/*
 * Free all of the driver resources and detach submodules.  Should be called
 * without the lock held.
 */
int
mps_free(struct mps_softc *sc)
{
	int error, dead = 0;

	if (sc->xnu.freed)		/* XNU: once */
		return (0);
	mps_dprint(sc, MPS_INIT, "%s entered\n", __func__);
	/* XNU: never started (StartController failed): the hook list is global */
	config_intrhook_disestablish(&sc->mps_ich);
	/* Turn off the watchdog */
	mps_lock(sc);
	sc->mps_flags |= MPS_FLAGS_SHUTDOWN;
	mps_unlock(sc);
	/* Lock must not be held for this */
	callout_drain(&sc->periodic);
	callout_drain(&sc->device_check_callout);
	callout_drain(&sc->xnu.gone_callout);	/* XNU */

	/* XNU: a reinit in progress gives up (MPS_FLAGS_SHUTDOWN); wait for it */
	if (sc->mps_tq != NULL) {
		taskqueue_free(sc->mps_tq);
		sc->mps_tq = NULL;
	}

	/*
	 * XNU: stop the IOC (message unit reset) before the outstanding
	 * commands and held ccbs are completed, so no buffer goes back to the
	 * OS while the IOC could still DMA into it; every ccb completes once.
	 * Suspended (mps_xnu_suspend): already stopped, registers left alone.
	 */
	mps_lock(sc);
	if ((sc->mps_flags & (MPS_FLAGS_XNU_GONE | MPS_FLAGS_XNU_SUSPENDED)) == 0 &&
	    sc->mps_flags & MPS_FLAGS_ATTACH_DONE) {
		/*
		 * XNU: our interrupt is already off (StopController: the
		 * family owns the source, and nothing like bus_teardown_intr
		 * would wait for a running handler after the queues are
		 * freed).  FreeBSD's is still live here, so what the IOC has
		 * completed is drained once, polled, as FreeBSD's NO_SLEEP
		 * waits do: it completes with its real status (D51).
		 */
		sc->xnu.final_drain = 1;
		mps_intr_locked(sc);
		sc->xnu.final_drain = 0;
		sc->xnu.stop_deadline_us = mps_os_uptime_us() +
		    (uint64_t)1000 * MPS_XNU_SLEEP_STOP_MS;
		if (mps_transition_ready(sc) != 0 &&
		    (sc->mps_flags & MPS_FLAGS_XNU_GONE) == 0) {
			/* the IOC may still DMA: no buffer back before this */
			mps_dprint(sc, MPS_INIT|MPS_FAULT, "the IOC did not "
			    "stop: bus mastering off\n");
			mps_os_controller_dead(sc->mps_dev);
			dead = 1;
		}
		sc->xnu.stop_deadline_us = 0;
	}
	mpssas_xnu_fail_all(sc);
	mps_unlock(sc);

	if (((error = mps_detach_log(sc)) != 0) ||
	    ((error = mps_detach_sas(sc)) != 0)) {
		mps_dprint(sc, MPS_INIT|MPS_FAULT, "failed to detach "
		    "subsystems, exit\n");
		return (error);
	}

	/* Put the IOC back in the READY state. */
	mps_lock(sc);
	sc->xnu.stop_deadline_us = mps_os_uptime_us() +
	    (uint64_t)1000 * MPS_XNU_SLEEP_STOP_MS;
	if (!dead && (error = mps_transition_ready(sc)) != 0 &&	/* XNU: free anyway */
	    (sc->mps_flags & (MPS_FLAGS_XNU_GONE | MPS_FLAGS_XNU_SUSPENDED)) == 0) {
		sc->xnu.stop_deadline_us = 0;
		mps_unlock(sc);
		return (error);
	}
	sc->xnu.stop_deadline_us = 0;
	mps_unlock(sc);

	if (sc->facts != NULL)
		free(sc->facts, M_MPT2);

	/*
	 * Free all buffers that are based on IOC Facts.  A Diag Reset may need
	 * to free these buffers too.
	 */
	mps_iocfacts_free(sc);

	/* XNU: no sysctl tree, no shutdown handler */

	mtx_destroy(&sc->mps_mtx);
	sc->xnu.freed = 1;
	mps_dprint(sc, MPS_INIT, "%s exit\n", __func__);

	return (0);
}

static __inline void
mps_complete_command(struct mps_softc *sc, struct mps_command *cm)
{
	MPS_FUNCTRACE(sc);

	if (cm == NULL) {
		mps_dprint(sc, MPS_ERROR, "Completing NULL command\n");
		return;
	}

	KASSERT(cm->cm_state == MPS_CM_STATE_INQUEUE,
	    ("command not inqueue, state = %u\n", cm->cm_state));
	cm->cm_state = MPS_CM_STATE_BUSY; 
	if (cm->cm_flags & MPS_CM_FLAGS_POLLED)
		cm->cm_flags |= MPS_CM_FLAGS_COMPLETE;

	if (cm->cm_complete != NULL) {
		mps_dprint(sc, MPS_TRACE,
			   "%s cm %p calling cm_complete %p data %p reply %p\n",
			   __func__, cm, cm->cm_complete, cm->cm_complete_data,
			   cm->cm_reply);
		cm->cm_complete(sc, cm);
	}

	if (cm->cm_flags & MPS_CM_FLAGS_WAKEUP) {
		mps_dprint(sc, MPS_TRACE, "waking up %p\n", cm);
		wakeup(cm);
	}

	if (cm->cm_sc->io_cmds_active != 0) {
		cm->cm_sc->io_cmds_active--;
	} else {
		mps_dprint(sc, MPS_ERROR, "Warning: io_cmds_active is "
		    "out of sync - resynching to 0\n");
	}
}

static void
mps_sas_log_info(struct mps_softc *sc , u32 log_info)
{
	union loginfo_type {
		u32     loginfo;
		struct {
			u32     subcode:16;
			u32     code:8;
			u32     originator:4;
			u32     bus_type:4;
		} dw;
	};
	union loginfo_type sas_loginfo;
	char *originator_str = NULL;

	sas_loginfo.loginfo = log_info;
	if (sas_loginfo.dw.bus_type != 3 /*SAS*/)
		return;

	/* each nexus loss loginfo */
	if (log_info == 0x31170000)
		return;

	/* eat the loginfos associated with task aborts */
	if ((log_info == 30050000 || log_info ==
	    0x31140000 || log_info == 0x31130000))
		return;

	switch (sas_loginfo.dw.originator) {
	case 0:
		originator_str = "IOP";
		break;
	case 1:
		originator_str = "PL";
		break;
	case 2:
		originator_str = "IR";
		break;
}

	mps_dprint(sc, MPS_LOG, "log_info(0x%08x): originator(%s), "
	"code(0x%02x), sub_code(0x%04x)\n", log_info,
	originator_str, sas_loginfo.dw.code,
	sas_loginfo.dw.subcode);
}

static void
mps_display_reply_info(struct mps_softc *sc, uint8_t *reply)
{
	MPI2DefaultReply_t *mpi_reply;
	u16 sc_status;

	mpi_reply = (MPI2DefaultReply_t*)reply;
	sc_status = le16toh(mpi_reply->IOCStatus);
	if (sc_status & MPI2_IOCSTATUS_FLAG_LOG_INFO_AVAILABLE)
		mps_sas_log_info(sc, le32toh(mpi_reply->IOCLogInfo));
}
void
mps_intr(void *data)
{
	struct mps_softc *sc;
	uint32_t status;

	sc = (struct mps_softc *)data;
	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);

	/*
	 * Check interrupt status register to flush the bus.  This is
	 * needed for both INTx interrupts and driver-driven polling
	 */
	status = mps_regread(sc, MPI2_HOST_INTERRUPT_STATUS_OFFSET);
	if ((status & MPI2_HIS_REPLY_DESCRIPTOR_INTERRUPT) == 0)
		return;

	mps_lock(sc);
	mps_intr_locked(data);
	mps_unlock(sc);
	return;
}

/*
 * In theory, MSI/MSIX interrupts shouldn't need to read any registers on the
 * chip.  Hopefully this theory is correct.
 */
void
mps_intr_msi(void *data)
{
	struct mps_softc *sc;

	sc = (struct mps_softc *)data;
	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);
	mps_lock(sc);
	mps_intr_locked(data);
	mps_unlock(sc);
	return;
}

/*
 * XNU: reply-descriptor hardening.  A SMID out of range, or one whose
 * command is not posted (a duplicate or stale completion), is logged,
 * counted and ignored instead of completing a free command.
 */
static int
mps_xnu_bad_smid(struct mps_softc *sc, u_int smid)
{

	if (smid == 0 || smid >= (u_int)sc->num_reqs) {
		mps_dprint(sc, MPS_ERROR, "reply for invalid SMID %u "
		    "(%d commands), dropped\n", smid, sc->num_reqs);
		sc->xnu.stats.bad_replies++;
		return (1);
	}
	if (sc->commands[smid].cm_state != MPS_CM_STATE_INQUEUE &&
	    (sc->commands[smid].cm_flags & MPS_CM_FLAGS_XNU_TM_TIMEDOUT)) {
		mps_dprint(sc, MPS_RECOVERY, "late reply for timed-out task "
		    "management SMID %u ignored (reinit pending)\n", smid);
		sc->xnu.stats.tm_late_replies++;
		return (1);
	}
	if (sc->commands[smid].cm_state != MPS_CM_STATE_INQUEUE) {
		mps_dprint(sc, MPS_ERROR|MPS_RECOVERY, "stale reply for idle "
		    "SMID %u (state %u), ignored\n", smid,
		    sc->commands[smid].cm_state);
		sc->xnu.stats.stale_replies++;
		return (1);
	}
	return (0);
}

/*
 * The locking is overly broad and simplistic, but easy to deal with for now.
 */
void
mps_intr_locked(void *data)
{
	MPI2_REPLY_DESCRIPTORS_UNION *desc;
	MPI2_DIAG_RELEASE_REPLY *rel_rep;
	mps_fw_diagnostic_buffer_t *pBuffer;
	struct mps_softc *sc;
	struct mps_command *cm = NULL;
	uint64_t tdesc;
	uint8_t flags;
	u_int pq;

	sc = (struct mps_softc *)data;

	/* XNU: the queues are being reset, or the controller is gone */
	if (sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE))
		return;

	pq = sc->replypostindex;
	mps_dprint(sc, MPS_TRACE,
	    "%s sc %p starting with replypostindex %u\n", 
	    __func__, sc, sc->replypostindex);

	for ( ;; ) {
		cm = NULL;
		desc = &sc->post_queue[sc->replypostindex];

		/*
		 * Copy and clear out the descriptor so that any reentry will
		 * immediately know that this descriptor has already been
		 * looked at.  There is unfortunate casting magic because the
		 * MPI API doesn't have a cardinal 64bit type.
		 */
		tdesc = 0xffffffffffffffff;
		tdesc = atomic_swap_64((uint64_t *)desc, tdesc);
		desc = (MPI2_REPLY_DESCRIPTORS_UNION *)&tdesc;

		flags = desc->Default.ReplyFlags &
		    MPI2_RPY_DESCRIPT_FLAGS_TYPE_MASK;
		if ((flags == MPI2_RPY_DESCRIPT_FLAGS_UNUSED)
		 || (le32toh(desc->Words.High) == 0xffffffff))
			break;

		/* increment the replypostindex now, so that event handlers
		 * and cm completion handlers which decide to do a diag
		 * reset can zero it without it getting incremented again
		 * afterwards, and we break out of this loop on the next
		 * iteration since the reply post queue has been cleared to
		 * 0xFF and all descriptors look unused (which they are).
		 */
		if (++sc->replypostindex >= sc->pqdepth)
			sc->replypostindex = 0;

		switch (flags) {
		case MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS:
			if (mps_xnu_bad_smid(sc,	/* XNU */
			    le16toh(desc->SCSIIOSuccess.SMID)))
				break;
			cm = &sc->commands[le16toh(desc->SCSIIOSuccess.SMID)];
			cm->cm_reply = NULL;
			break;
		case MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY:
		{
			uint32_t baddr;
			uint8_t *reply;

			/*
			 * Re-compose the reply address from the address
			 * sent back from the chip.  The ReplyFrameAddress
			 * is the lower 32 bits of the physical address of
			 * particular reply frame.  Convert that address to
			 * host format, and then use that to provide the
			 * offset against the virtual address base
			 * (sc->reply_frames).
			 */
			baddr = le32toh(desc->AddressReply.ReplyFrameAddress);
			/*
			 * Make sure the reply we got back is in a valid
			 * range.  XNU: and on a frame boundary; if not, log,
			 * count and drop it instead of panicking (the frame
			 * cannot be given back to the free queue).
			 */
			if ((baddr - (uint32_t)sc->reply_busaddr) >=
			    (uint32_t)(sc->fqdepth * sc->replyframesz) ||
			    (baddr - (uint32_t)sc->reply_busaddr) %
			    sc->replyframesz != 0) {
				mps_dprint(sc, MPS_ERROR, "%s: reply address "
				    "%#x out of range (frames %#x, %d x %d), "
				    "SMID %u dropped\n", __func__, baddr,
				    (uint32_t)sc->reply_busaddr, sc->fqdepth,
				    sc->replyframesz,
				    le16toh(desc->AddressReply.SMID));
				sc->xnu.stats.bad_replies++;
				break;
			}
			reply = sc->reply_frames +
				(baddr - ((uint32_t)sc->reply_busaddr));
			if (le16toh(desc->AddressReply.SMID) == 0) {
				if (((MPI2_DEFAULT_REPLY *)reply)->Function ==
				    MPI2_FUNCTION_DIAG_BUFFER_POST) {
					/*
					 * If SMID is 0 for Diag Buffer Post,
					 * this implies that the reply is due to
					 * a release function with a status that
					 * the buffer has been released.  Set
					 * the buffer flags accordingly.
					 */
					rel_rep =
					    (MPI2_DIAG_RELEASE_REPLY *)reply;
					if ((le16toh(rel_rep->IOCStatus) &
					    MPI2_IOCSTATUS_MASK) ==
					    MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED)
					{
						pBuffer =
						    &sc->fw_diag_buffer_list[
						    rel_rep->BufferType];
						pBuffer->valid_data = TRUE;
						pBuffer->owned_by_firmware =
						    FALSE;
						pBuffer->immediate = FALSE;
					}
				} else
					mps_dispatch_event(sc, baddr,
					    (MPI2_EVENT_NOTIFICATION_REPLY *)
					    reply);
			} else if (mps_xnu_bad_smid(sc,	/* XNU */
			    le16toh(desc->AddressReply.SMID))) {
				mps_free_reply(sc, baddr);
			} else {
				/*
				 * Ignore commands not in INQUEUE state
				 * since they've already been completed
				 * via another path.
				 */
				cm = &sc->commands[
				    le16toh(desc->AddressReply.SMID)];
				if (cm->cm_state == MPS_CM_STATE_INQUEUE) {
					cm->cm_reply = reply;
					cm->cm_reply_data = le32toh(
					    desc->AddressReply.ReplyFrameAddress);
				} else {
					mps_dprint(sc, MPS_RECOVERY,
					    "Bad state for ADDRESS_REPLY status,"
					    " ignoring state %d cm %p\n",
					    cm->cm_state, cm);
				}
			}
			break;
		}
		case MPI2_RPY_DESCRIPT_FLAGS_TARGETASSIST_SUCCESS:
		case MPI2_RPY_DESCRIPT_FLAGS_TARGET_COMMAND_BUFFER:
		case MPI2_RPY_DESCRIPT_FLAGS_RAID_ACCELERATOR_SUCCESS:
		default:
			/* Unhandled */
			mps_dprint(sc, MPS_ERROR, "Unhandled reply 0x%x\n",
			    desc->Default.ReplyFlags);
			cm = NULL;
			break;
		}
		

		if (cm != NULL) {
			// Print Error reply frame
			if (cm->cm_reply)
				mps_display_reply_info(sc,cm->cm_reply);
			mps_complete_command(sc, cm);
		}
	}

	if (pq != sc->replypostindex) {
		mps_dprint(sc, MPS_TRACE, "%s sc %p writing postindex %d\n",
		    __func__, sc, sc->replypostindex);
		mps_regwrite(sc, MPI2_REPLY_POST_HOST_INDEX_OFFSET,
		    sc->replypostindex);
	}

	return;
}

static void
mps_dispatch_event(struct mps_softc *sc, uintptr_t data,
    MPI2_EVENT_NOTIFICATION_REPLY *reply)
{
	struct mps_event_handle *eh;
	int event, handled = 0;

	event = le16toh(reply->Event);
	TAILQ_FOREACH(eh, &sc->event_list, eh_list) {
		if (isset(eh->mask, event)) {
			eh->callback(sc, data, reply);
			handled++;
		}
	}

	if (handled == 0)
		mps_dprint(sc, MPS_EVENT, "Unhandled event 0x%x\n", le16toh(event));

	/*
	 * XNU: acknowledge events that ask for it (FreeBSD never sends
	 * EVENT_ACK; the pre-FreeBSD driver did, as Linux mpt2sas does).
	 */
	sc->xnu.stats.events++;
	mps_xnu_event_ack(sc, reply);

	/*
	 * This is the only place that the event/reply should be freed.
	 * Anything wanting to hold onto the event data should have
	 * already copied it into their own storage.
	 */
	mps_free_reply(sc, data);
}

static void
mps_reregister_events_complete(struct mps_softc *sc, struct mps_command *cm)
{
	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);

	if (cm->cm_reply)
		MPS_DPRINT_EVENT(sc, generic,
			(MPI2_EVENT_NOTIFICATION_REPLY *)cm->cm_reply);

	mps_free_command(sc, cm);

	/* next, send a port enable */
	mpssas_startup(sc);
}

/*
 * For both register_events and update_events, the caller supplies a bitmap
 * of events that it _wants_.  These functions then turn that into a bitmask
 * suitable for the controller.
 */
int
mps_register_events(struct mps_softc *sc, u32 *mask,
    mps_evt_callback_t *cb, void *data, struct mps_event_handle **handle)
{
	struct mps_event_handle *eh;
	int error = 0;

	eh = malloc(sizeof(struct mps_event_handle), M_MPT2, M_WAITOK|M_ZERO);
	if (eh == NULL)		/* XNU: IOMalloc can fail */
		return (ENOMEM);
	eh->callback = cb;
	eh->data = data;
	TAILQ_INSERT_TAIL(&sc->event_list, eh, eh_list);
	if (mask != NULL)
		error = mps_update_events(sc, eh, mask);
	*handle = eh;

	return (error);
}

int
mps_update_events(struct mps_softc *sc, struct mps_event_handle *handle,
    u32 *mask)
{
	MPI2_EVENT_NOTIFICATION_REQUEST *evtreq;
	MPI2_EVENT_NOTIFICATION_REPLY *reply = NULL;
	struct mps_command *cm;
	int error, i;

	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);

	if ((mask != NULL) && (handle != NULL))
		bcopy(mask, &handle->mask[0], sizeof(u32) * 
				MPI2_EVENT_NOTIFY_EVENTMASK_WORDS);
    
	for (i = 0; i < MPI2_EVENT_NOTIFY_EVENTMASK_WORDS; i++)
		sc->event_mask[i] = -1;

	for (i = 0; i < MPI2_EVENT_NOTIFY_EVENTMASK_WORDS; i++)
		sc->event_mask[i] &= ~handle->mask[i];

	if ((cm = mps_alloc_command(sc)) == NULL)
		return (EBUSY);
	evtreq = (MPI2_EVENT_NOTIFICATION_REQUEST *)cm->cm_req;
	evtreq->Function = MPI2_FUNCTION_EVENT_NOTIFICATION;
	evtreq->MsgFlags = 0;
	evtreq->SASBroadcastPrimitiveMasks = 0;
#ifdef MPS_DEBUG_ALL_EVENTS
	{
		u_char fullmask[16];
		memset(fullmask, 0x00, 16);
		bcopy(fullmask, &evtreq->EventMasks[0], sizeof(u32) * 
				MPI2_EVENT_NOTIFY_EVENTMASK_WORDS);
	}
#else
        for (i = 0; i < MPI2_EVENT_NOTIFY_EVENTMASK_WORDS; i++)
                evtreq->EventMasks[i] =
                    htole32(sc->event_mask[i]);
#endif
	cm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	cm->cm_data = NULL;

	error = mps_wait_command(sc, &cm, 60, 0);
	if (cm != NULL)
		reply = (MPI2_EVENT_NOTIFICATION_REPLY *)cm->cm_reply;
	if ((reply == NULL) ||
	    (reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS)
		error = ENXIO;

	if (reply)
		MPS_DPRINT_EVENT(sc, generic, reply);

	mps_dprint(sc, MPS_TRACE, "%s finished error %d\n", __func__, error);

	if (cm != NULL)
		mps_free_command(sc, cm);
	return (error);
}

static int
mps_reregister_events(struct mps_softc *sc)
{
	MPI2_EVENT_NOTIFICATION_REQUEST *evtreq;
	struct mps_command *cm;
	struct mps_event_handle *eh;
	int error, i;

	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);

	/* first, reregister events */

	for (i = 0; i < MPI2_EVENT_NOTIFY_EVENTMASK_WORDS; i++)
		sc->event_mask[i] = -1;

	TAILQ_FOREACH(eh, &sc->event_list, eh_list) {
		for (i = 0; i < MPI2_EVENT_NOTIFY_EVENTMASK_WORDS; i++)
			sc->event_mask[i] &= ~eh->mask[i];
	}

	if ((cm = mps_alloc_command(sc)) == NULL)
		return (EBUSY);
	evtreq = (MPI2_EVENT_NOTIFICATION_REQUEST *)cm->cm_req;
	evtreq->Function = MPI2_FUNCTION_EVENT_NOTIFICATION;
	evtreq->MsgFlags = 0;
	evtreq->SASBroadcastPrimitiveMasks = 0;
#ifdef MPS_DEBUG_ALL_EVENTS
	{
		u_char fullmask[16];
		memset(fullmask, 0x00, 16);
		bcopy(fullmask, &evtreq->EventMasks[0], sizeof(u32) *
			MPI2_EVENT_NOTIFY_EVENTMASK_WORDS);
	}
#else
        for (i = 0; i < MPI2_EVENT_NOTIFY_EVENTMASK_WORDS; i++)
                evtreq->EventMasks[i] =
                    htole32(sc->event_mask[i]);
#endif
	cm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	cm->cm_data = NULL;
	cm->cm_complete = mps_reregister_events_complete;

	error = mps_map_command(sc, cm);

	mps_dprint(sc, MPS_TRACE, "%s finished with error %d\n", __func__,
	    error);
	return (error);
}

void
mps_deregister_events(struct mps_softc *sc, struct mps_event_handle *handle)
{

	if (handle == NULL)	/* XNU: registration failed (attach) */
		return;

	TAILQ_REMOVE(&sc->event_list, handle, eh_list);
	free(handle, M_MPT2);
}

/*
 * Add a chain element as the next SGE for the specified command.
 * Reset cm_sge and cm_sgesize to indicate all the available space.
 */
static int
mps_add_chain(struct mps_command *cm)
{
	MPI2_SGE_CHAIN64 *sgc;
	struct mps_chain *chain;
	u_int space;

	if (cm->cm_sglsize < MPS_SGC_SIZE) {
		panic("MPS: Need SGE Error Code\n");
		return (EFBIG);	/* XNU: panic() returns: no overrun (D18) */
	}

	chain = mps_alloc_chain(cm->cm_sc);
	if (chain == NULL)
		return (ENOBUFS);

	space = cm->cm_sc->reqframesz;

	/*
	 * Note: a double-linked list is used to make it easier to
	 * walk for debugging.
	 */
	TAILQ_INSERT_TAIL(&cm->cm_chain_list, chain, chain_link);

	sgc = (MPI2_SGE_CHAIN64 *)&cm->cm_sge->MpiChain;
	sgc->Length = htole16(space);
	sgc->NextChainOffset = 0;
	/* TODO Looks like bug in Setting sgc->Flags. 
	 *	sgc->Flags = ( MPI2_SGE_FLAGS_CHAIN_ELEMENT | MPI2_SGE_FLAGS_64_BIT_ADDRESSING |
	 *	            MPI2_SGE_FLAGS_SYSTEM_ADDRESS) << MPI2_SGE_FLAGS_SHIFT
	 *	This is fine.. because we are not using simple element. In case of 
	 *	MPI2_SGE_CHAIN64, we have separate Length and Flags field.
 	 */
	sgc->Flags = MPI2_SGE_FLAGS_CHAIN_ELEMENT | MPI2_SGE_FLAGS_64_BIT_ADDRESSING;
	sgc->Address.High = htole32(chain->chain_busaddr >> 32);
	sgc->Address.Low = htole32(chain->chain_busaddr);

	cm->cm_sge = (MPI2_SGE_IO_UNION *)&chain->chain->MpiSimple;
	cm->cm_sglsize = space;
	return (0);
}

/*
 * Add one scatter-gather element (chain, simple, transaction context)
 * to the scatter-gather list for a command.  Maintain cm_sglsize and
 * cm_sge as the remaining size and pointer to the next SGE to fill
 * in, respectively.
 */
int
mps_push_sge(struct mps_command *cm, void *sgep, size_t len, int segsleft)
{
	MPI2_SGE_TRANSACTION_UNION *tc = sgep;
	MPI2_SGE_SIMPLE64 *sge = sgep;
	int error, type;
	uint32_t saved_buf_len, saved_address_low, saved_address_high;

	type = (tc->Flags & MPI2_SGE_FLAGS_ELEMENT_MASK);

#ifdef INVARIANTS
	switch (type) {
	case MPI2_SGE_FLAGS_TRANSACTION_ELEMENT: {
		if (len != tc->DetailsLength + 4)
			panic("TC %p length %u or %zu?", tc,
			    tc->DetailsLength + 4, len);
		}
		break;
	case MPI2_SGE_FLAGS_CHAIN_ELEMENT:
		/* Driver only uses 64-bit chain elements */
		if (len != MPS_SGC_SIZE)
			panic("CHAIN %p length %u or %zu?", sgep,
			    MPS_SGC_SIZE, len);
		break;
	case MPI2_SGE_FLAGS_SIMPLE_ELEMENT:
		/* Driver only uses 64-bit SGE simple elements */
		if (len != MPS_SGE64_SIZE)
			panic("SGE simple %p length %u or %zu?", sge,
			    MPS_SGE64_SIZE, len);
		if (((le32toh(sge->FlagsLength) >> MPI2_SGE_FLAGS_SHIFT) &
		    MPI2_SGE_FLAGS_ADDRESS_SIZE) == 0)
			panic("SGE simple %p not marked 64-bit?", sge);

		break;
	default:
		panic("Unexpected SGE %p, flags %02x", tc, tc->Flags);
	}
#endif

	/*
	 * case 1: 1 more segment, enough room for it
	 * case 2: 2 more segments, enough room for both
	 * case 3: >=2 more segments, only enough room for 1 and a chain
	 * case 4: >=1 more segment, enough room for only a chain
	 * case 5: >=1 more segment, no room for anything (error)
         */

	/*
	 * There should be room for at least a chain element, or this
	 * code is buggy.  Case (5).
	 */
	if (cm->cm_sglsize < MPS_SGC_SIZE) {
		panic("MPS: Need SGE Error Code\n");
		return (EFBIG);	/* XNU: panic() returns: no overrun (D18) */
	}

	if (segsleft >= 1 && cm->cm_sglsize < len + MPS_SGC_SIZE) {
		/*
		 * 1 or more segment, enough room for only a chain.
		 * Hope the previous element wasn't a Simple entry
		 * that needed to be marked with
		 * MPI2_SGE_FLAGS_LAST_ELEMENT.  Case (4).
		 */
		if ((error = mps_add_chain(cm)) != 0)
			return (error);
	}

	if (segsleft >= 2 &&
	    cm->cm_sglsize < len + MPS_SGC_SIZE + MPS_SGE64_SIZE) {
		/*
		 * There are 2 or more segments left to add, and only
		 * enough room for 1 and a chain.  Case (3).
		 *
		 * Mark as last element in this chain if necessary.
		 */
		if (type == MPI2_SGE_FLAGS_SIMPLE_ELEMENT) {
			/* XNU: unsigned: 0x80 << 24 overflows int (UBSan) */
			sge->FlagsLength |= htole32(
			    (uint32_t)MPI2_SGE_FLAGS_LAST_ELEMENT << MPI2_SGE_FLAGS_SHIFT);
		}

		/*
		 * Add the item then a chain.  Do the chain now,
		 * rather than on the next iteration, to simplify
		 * understanding the code.
		 */
		cm->cm_sglsize -= len;
		bcopy(sgep, cm->cm_sge, len);
		cm->cm_sge = (MPI2_SGE_IO_UNION *)((uintptr_t)cm->cm_sge + len);
		return (mps_add_chain(cm));
	}

#ifdef INVARIANTS
	/* Case 1: 1 more segment, enough room for it. */
	if (segsleft == 1 && cm->cm_sglsize < len)
		panic("1 seg left and no room? %u versus %zu",
		    cm->cm_sglsize, len);

	/* Case 2: 2 more segments, enough room for both */
	if (segsleft == 2 && cm->cm_sglsize < len + MPS_SGE64_SIZE)
		panic("2 segs left and no room? %u versus %zu",
		    cm->cm_sglsize, len);
#endif

	if (segsleft == 1 && type == MPI2_SGE_FLAGS_SIMPLE_ELEMENT) {
		/*
		 * If this is a bi-directional request, need to account for that
		 * here.  Save the pre-filled sge values.  These will be used
		 * either for the 2nd SGL or for a single direction SGL.  If
		 * cm_out_len is non-zero, this is a bi-directional request, so
		 * fill in the OUT SGL first, then the IN SGL, otherwise just
		 * fill in the IN SGL.  Note that at this time, when filling in
		 * 2 SGL's for a bi-directional request, they both use the same
		 * DMA buffer (same cm command).
		 */
		saved_buf_len = le32toh(sge->FlagsLength) & 0x00FFFFFF;
		saved_address_low = sge->Address.Low;
		saved_address_high = sge->Address.High;
		if (cm->cm_out_len) {
			sge->FlagsLength = htole32(cm->cm_out_len |
			    ((uint32_t)(MPI2_SGE_FLAGS_SIMPLE_ELEMENT |
			    MPI2_SGE_FLAGS_END_OF_BUFFER |
			    MPI2_SGE_FLAGS_HOST_TO_IOC |
			    MPI2_SGE_FLAGS_64_BIT_ADDRESSING) <<
			    MPI2_SGE_FLAGS_SHIFT));
			cm->cm_sglsize -= len;
			bcopy(sgep, cm->cm_sge, len);
			cm->cm_sge = (MPI2_SGE_IO_UNION *)((uintptr_t)cm->cm_sge
			    + len);
		}
		saved_buf_len |=
		    ((uint32_t)(MPI2_SGE_FLAGS_SIMPLE_ELEMENT |
		    MPI2_SGE_FLAGS_END_OF_BUFFER |
		    MPI2_SGE_FLAGS_LAST_ELEMENT |
		    MPI2_SGE_FLAGS_END_OF_LIST |
		    MPI2_SGE_FLAGS_64_BIT_ADDRESSING) <<
		    MPI2_SGE_FLAGS_SHIFT);
		if (cm->cm_flags & MPS_CM_FLAGS_DATAIN) {
			saved_buf_len |=
			    ((uint32_t)(MPI2_SGE_FLAGS_IOC_TO_HOST) <<
			    MPI2_SGE_FLAGS_SHIFT);
		} else {
			saved_buf_len |=
			    ((uint32_t)(MPI2_SGE_FLAGS_HOST_TO_IOC) <<
			    MPI2_SGE_FLAGS_SHIFT);
		}
		sge->FlagsLength = htole32(saved_buf_len);
		sge->Address.Low = saved_address_low;
		sge->Address.High = saved_address_high;
	}

	cm->cm_sglsize -= len;
	bcopy(sgep, cm->cm_sge, len);
	cm->cm_sge = (MPI2_SGE_IO_UNION *)((uintptr_t)cm->cm_sge + len);
	return (0);
}

/*
 * Add one dma segment to the scatter-gather list for a command.
 */
int
mps_add_dmaseg(struct mps_command *cm, vm_paddr_t pa, size_t len, u_int flags,
    int segsleft)
{
	MPI2_SGE_SIMPLE64 sge;

	/*
	 * This driver always uses 64-bit address elements for simplicity.
	 */
	bzero(&sge, sizeof(sge));
	flags |= MPI2_SGE_FLAGS_SIMPLE_ELEMENT |
	    MPI2_SGE_FLAGS_64_BIT_ADDRESSING;
	sge.FlagsLength = htole32(len | (flags << MPI2_SGE_FLAGS_SHIFT));
	mps_from_u64(pa, &sge.Address);

	return (mps_push_sge(cm, &sge, sizeof sge, segsleft));
}

static void
mps_data_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct mps_softc *sc;
	struct mps_command *cm;
	u_int i, dir, sflags;

	cm = (struct mps_command *)arg;
	sc = cm->cm_sc;

	/*
	 * In this case, just print out a warning and let the chip tell the
	 * user they did the wrong thing.
	 */
	if ((cm->cm_max_segs != 0) && (nsegs > cm->cm_max_segs)) {
		mps_dprint(sc, MPS_ERROR,
			   "%s: warning: busdma returned %d segments, "
			   "more than the %d allowed\n", __func__, nsegs,
			   cm->cm_max_segs);
	}

	/*
	 * Set up DMA direction flags.  Bi-directional requests are also handled
	 * here.  In that case, both direction flags will be set.
	 */
	sflags = 0;
	if (cm->cm_flags & MPS_CM_FLAGS_SMP_PASS) {
		/*
		 * We have to add a special case for SMP passthrough, there
		 * is no easy way to generically handle it.  The first
		 * S/G element is used for the command (therefore the
		 * direction bit needs to be set).  The second one is used
		 * for the reply.  We'll leave it to the caller to make
		 * sure we only have two buffers.
		 */
		/*
		 * Even though the busdma man page says it doesn't make
		 * sense to have both direction flags, it does in this case.
		 * We have one s/g element being accessed in each direction.
		 */
		dir = BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD;

		/*
		 * Set the direction flag on the first buffer in the SMP
		 * passthrough request.  We'll clear it for the second one.
		 */
		sflags |= MPI2_SGE_FLAGS_DIRECTION |
			  MPI2_SGE_FLAGS_END_OF_BUFFER;
	} else if (cm->cm_flags & MPS_CM_FLAGS_DATAOUT) {
		sflags |= MPI2_SGE_FLAGS_HOST_TO_IOC;
		dir = BUS_DMASYNC_PREWRITE;
	} else
		dir = BUS_DMASYNC_PREREAD;

	for (i = 0; i < nsegs; i++) {
		if ((cm->cm_flags & MPS_CM_FLAGS_SMP_PASS) && (i != 0)) {
			sflags &= ~MPI2_SGE_FLAGS_DIRECTION;
		}
		error = mps_add_dmaseg(cm, segs[i].ds_addr, segs[i].ds_len,
		    sflags, nsegs - i);
		if (error != 0) {
			/* Resource shortage, roll back! */
			if (ratecheck(&sc->lastfail, &mps_chainfail_interval))
				mps_dprint(sc, MPS_INFO, "Out of chain frames, "
				    "consider increasing hw.mps.max_chains.\n");
			/* XNU: EFBIG, the SGL does not fit (D18): fail it */
			cm->cm_flags |= (error == EFBIG) ?
			    MPS_CM_FLAGS_XNU_SGL_FAILED :
			    MPS_CM_FLAGS_CHAIN_FAILED;
			/*
			 * mpr_complete_command can only be called on commands
			 * that are in the queue. Since this is an error path
			 * which gets called before we enqueue, update the state
			 * to meet this requirement before we complete it.
			 */
			cm->cm_state = MPS_CM_STATE_INQUEUE;
			mps_complete_command(sc, cm);
			return;
		}
	}

	bus_dmamap_sync(sc->buffer_dmat, cm->cm_dmamap, dir);
	mps_enqueue_request(sc, cm);

	return;
}

/*
 * This is the routine to enqueue commands ansynchronously.
 * Note that the only error path here is from bus_dmamap_load(), which can
 * return EINPROGRESS if it is waiting for resources.  Other than this, it's
 * assumed that if you have a command in-hand, then you have enough credits
 * to use it.
 */
int
mps_map_command(struct mps_softc *sc, struct mps_command *cm)
{
	int error = 0;

	/* XNU: no MPS_CM_FLAGS_USE_UIO (user commands) */
	if (cm->cm_flags & MPS_CM_FLAGS_USE_CCB) {
		error = bus_dmamap_load_ccb(sc->buffer_dmat, cm->cm_dmamap,
		    cm->cm_data, mps_data_cb, cm, 0);
	} else if ((cm->cm_data != NULL) && (cm->cm_length != 0)) {
		error = bus_dmamap_load(sc->buffer_dmat, cm->cm_dmamap,
		    cm->cm_data, cm->cm_length, mps_data_cb, cm, 0);
	} else {
		/* Add a zero-length element as needed */
		if (cm->cm_sge != NULL)
			mps_add_dmaseg(cm, 0, 0, 0, 1);
		mps_enqueue_request(sc, cm);	
	}

	return (error);
}

/*
 * This is the routine to enqueue commands synchronously.  An error of
 * EINPROGRESS from mps_map_command() is ignored since the command will
 * be executed and enqueued automatically.  Other errors come from msleep().
 */
int
mps_wait_command(struct mps_softc *sc, struct mps_command **cmp, int timeout,
    int sleep_flag)
{
	int error, rc;
	struct timeval cur_time, start_time;
	struct mps_command *cm = *cmp;

	if (sc->mps_flags & MPS_FLAGS_DIAGRESET) 
		return  EBUSY;
	/* XNU: stopped, unplugged or asleep: no reply will come */
	if (sc->mps_flags & (MPS_FLAGS_SHUTDOWN | MPS_FLAGS_XNU_GONE |
	    MPS_FLAGS_XNU_SUSPENDED))
		return (ENXIO);

	cm->cm_complete = NULL;
	cm->cm_flags |= MPS_CM_FLAGS_POLLED;
	error = mps_map_command(sc, cm);
	if ((error != 0) && (error != EINPROGRESS))
		return (error);

	/*
	 * Check for context and wait for 50 mSec at a time until time has
	 * expired or the command has finished.  If msleep can't be used, need
	 * to poll.
	 */
	if (curthread->td_no_sleeping != 0)
		sleep_flag = NO_SLEEP;
	getmicrouptime(&start_time);
	if (mtx_owned(&sc->mps_mtx) && sleep_flag == CAN_SLEEP) {
		cm->cm_flags |= MPS_CM_FLAGS_WAKEUP;
		error = msleep(cm, &sc->mps_mtx, 0, "mpswait", timeout*hz);
		if (error == EWOULDBLOCK) {
			/*
			 * Record the actual elapsed time in the case of a
			 * timeout for the message below.
			 */
			getmicrouptime(&cur_time);
			timevalsub(&cur_time, &start_time);
		}
	} else {
		while ((cm->cm_flags & MPS_CM_FLAGS_COMPLETE) == 0) {
			mps_intr_locked(sc);
			if (sleep_flag == CAN_SLEEP)
				pause("mpswait", hz/20);
			else
				DELAY(50000);
		
			getmicrouptime(&cur_time);
			timevalsub(&cur_time, &start_time);
			if (cur_time.tv_sec > timeout) {
				error = EWOULDBLOCK;
				break;
			}
		}
	}

	if (error == EWOULDBLOCK) {
		if (cm->cm_timeout_handler == NULL) {
			mps_dprint(sc, MPS_FAULT, "Calling Reinit from %s, timeout=%d,"
			    " elapsed=%jd\n", __func__, timeout,
			    (intmax_t)cur_time.tv_sec);
			rc = (mps_reinit)(sc);	/* XNU: the reset (D54) */
			mps_dprint(sc, MPS_FAULT, "Reinit %s\n", (rc == 0) ? "success" :
			    "failed");
		} else
			cm->cm_timeout_handler(sc, cm);
		if (sc->mps_flags & MPS_FLAGS_REALLOCATED) {
			/*
			 * Tell the caller that we freed the command in a
			 * reinit.
			 */
			*cmp = NULL;
		}
		error = ETIMEDOUT;
	}
	return (error);
}
