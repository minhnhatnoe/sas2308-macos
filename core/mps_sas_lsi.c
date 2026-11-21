/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
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





/* For Hashed SAS Address creation for SATA Drives */
#define MPT2SAS_SN_LEN 20
#define MPT2SAS_MN_LEN 40

struct mps_fw_event_work {
	u16			event;
	void			*event_data;
	TAILQ_ENTRY(mps_fw_event_work)	ev_link;
};

union _sata_sas_address {
	u8 wwid[8];
	struct {
		u32 high;
		u32 low;
	} word;
};

/*
 * define the IDENTIFY DEVICE structure
 */
struct _ata_identify_device_data {
	u16 reserved1[10];	/* 0-9 */
	u16 serial_number[10];	/* 10-19 */
	u16 reserved2[7];	/* 20-26 */
	u16 model_number[20];	/* 27-46*/
	u16 reserved3[170];	/* 47-216 */
	u16 rotational_speed;	/* 217 */
	u16 reserved4[38];	/* 218-255 */
};

static u32 event_count;
static void mpssas_fw_work(struct mps_softc *sc,
    struct mps_fw_event_work *fw_event);
static void mpssas_fw_event_free(struct mps_softc *,
    struct mps_fw_event_work *);
static int mpssas_add_device(struct mps_softc *sc, u16 handle, u8 linkrate);
static int mpssas_get_sata_identify(struct mps_softc *sc, u16 handle,
    Mpi2SataPassthroughReply_t *mpi_reply, char *id_buffer, int sz,
    u32 devinfo);
static void mpssas_ata_id_complete(struct mps_softc *, struct mps_command *);
static void mpssas_ata_id_timeout(struct mps_softc *, struct mps_command *);

void
mpssas_evt_handler(struct mps_softc *sc, uintptr_t data,
    MPI2_EVENT_NOTIFICATION_REPLY *event)
{
	struct mps_fw_event_work *fw_event;
	u16 sz;

	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);
	MPS_DPRINT_EVENT(sc, sas, event);
	/* XNU: no event recording (mps_user.c ioctl) */

	fw_event = malloc(sizeof(struct mps_fw_event_work), M_MPT2,
	     M_ZERO|M_NOWAIT);
	if (!fw_event) {
		printf("%s: allocate failed for fw_event\n", __func__);
		return;
	}
	sz = le16toh(event->EventDataLength) * 4;
	fw_event->event_data = malloc(sz, M_MPT2, M_ZERO|M_NOWAIT);
	if (!fw_event->event_data) {
		printf("%s: allocate failed for event_data\n", __func__);
		free(fw_event, M_MPT2);
		return;
	}

	bcopy(event->EventData, fw_event->event_data, sz);
	fw_event->event = event->Event;
	if ((event->Event == MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST ||
	    event->Event == MPI2_EVENT_SAS_ENCL_DEVICE_STATUS_CHANGE ||
	    event->Event == MPI2_EVENT_IR_CONFIGURATION_CHANGE_LIST) &&
	    sc->track_mapping_events)
		sc->pending_map_events++;

	/*
	 * When wait_for_port_enable flag is set, make sure that all the events
	 * are processed. Increment the startup_refcount and decrement it after
	 * events are processed.
	 */
	if ((event->Event == MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST) &&
	    sc->wait_for_port_enable)	/* XNU: no IR events handled */
		mpssas_startup_increment(sc->sassc);

	TAILQ_INSERT_TAIL(&sc->sassc->ev_queue, fw_event, ev_link);
	taskqueue_enqueue(sc->sassc->ev_tq, &sc->sassc->ev_task);

}

/*
 * XNU: queue a pseudo event (MPS_XNU_EVENT_*) behind the firmware's events,
 * as mpssas_evt_handler does; mps_mtx held.
 */
int
mpssas_xnu_queue_work(struct mps_softc *sc, uint16_t event, const void *data,
    size_t len)
{
	struct mps_fw_event_work *fw_event;

	if (sc->sassc == NULL || sc->sassc->ev_tq == NULL)
		return (ENXIO);
	fw_event = malloc(sizeof(struct mps_fw_event_work), M_MPT2,
	     M_ZERO|M_NOWAIT);
	if (!fw_event)
		return (ENOMEM);
	fw_event->event_data = malloc(len ? len : 1, M_MPT2, M_ZERO|M_NOWAIT);
	if (!fw_event->event_data) {
		free(fw_event, M_MPT2);
		return (ENOMEM);
	}
	if (len)
		bcopy(data, fw_event->event_data, len);
	fw_event->event = event;
	TAILQ_INSERT_TAIL(&sc->sassc->ev_queue, fw_event, ev_link);
	taskqueue_enqueue(sc->sassc->ev_tq, &sc->sassc->ev_task);
	return (0);
}

static void
mpssas_fw_event_free(struct mps_softc *sc, struct mps_fw_event_work *fw_event)
{

	free(fw_event->event_data, M_MPT2);
	free(fw_event, M_MPT2);
}

/**
 * _mps_fw_work - delayed task for processing firmware events
 * @sc: per adapter object
 * @fw_event: The fw_event_work object
 * Context: user.
 *
 * Return nothing.
 */
static void
mpssas_fw_work(struct mps_softc *sc, struct mps_fw_event_work *fw_event)
{
	struct mpssas_softc *sassc;
	/*
	 * XNU: 0xFF01/0xFF02 are not MPI2 events (MPI2 codes end at 0x7F) but
	 * our own work items (MPS_XNU_EVENT_READD, MPS_XNU_EVENT_TEMP every
	 * 30 s): logged at xinfo, not as IOC events.
	 */
	const int evlvl = (fw_event->event == MPS_XNU_EVENT_READD ||
	    fw_event->event == MPS_XNU_EVENT_TEMP) ? MPS_XINFO : MPS_EVENT;
	sassc = sc->sassc;

	mps_dprint(sc, evlvl, "(%d)->(%s) Working on  Event: [%x] %s\n",
			event_count++,__func__,fw_event->event,
			mps_describe_table(mps_event_names, fw_event->event));
	switch (fw_event->event) {
	case MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST: 
	{
		MPI2_EVENT_DATA_SAS_TOPOLOGY_CHANGE_LIST *data;
		MPI2_EVENT_SAS_TOPO_PHY_ENTRY *phy;
		int i;

		data = (MPI2_EVENT_DATA_SAS_TOPOLOGY_CHANGE_LIST *)
		    fw_event->event_data;

		/* XNU: no mps_mapping.c */

		for (i = 0; i < data->NumEntries; i++) {
			phy = &data->PHY[i];
			switch (phy->PhyStatus & MPI2_EVENT_SAS_TOPO_RC_MASK) {
			case MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED:
				if (mpssas_add_device(sc,
				    le16toh(phy->AttachedDevHandle),
				    phy->LinkRate)){
					mps_dprint(sc, MPS_ERROR, "%s: "
					    "failed to add device with handle "
					    "0x%x\n", __func__,
					    le16toh(phy->AttachedDevHandle));
					mpssas_prepare_remove(sassc, le16toh(
						phy->AttachedDevHandle));
				}
				break;
			case MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING:
				mps_xnu_readd_cancel(sc, le16toh(	/* XNU */
				    phy->AttachedDevHandle));
				mpssas_prepare_remove(sassc,le16toh( 
					phy->AttachedDevHandle));
				break;
			case MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED:
			case MPI2_EVENT_SAS_TOPO_RC_NO_CHANGE:
			case MPI2_EVENT_SAS_TOPO_RC_DELAY_NOT_RESPONDING:
			default:
				break;
			}
		}
		/*
		 * refcount was incremented for this event in
		 * mpssas_evt_handler.  Decrement it here because the event has
		 * been processed.
		 */
		mpssas_startup_decrement(sassc);
		break;
	}
	case MPI2_EVENT_SAS_DISCOVERY:
	{
		MPI2_EVENT_DATA_SAS_DISCOVERY *data;

		data = (MPI2_EVENT_DATA_SAS_DISCOVERY *)fw_event->event_data;

		if (data->ReasonCode & MPI2_EVENT_SAS_DISC_RC_STARTED)
			mps_dprint(sc, MPS_TRACE,"SAS discovery start event\n");
		if (data->ReasonCode & MPI2_EVENT_SAS_DISC_RC_COMPLETED) {
			mps_dprint(sc, MPS_TRACE,"SAS discovery stop event\n");
			sassc->flags &= ~MPSSAS_IN_DISCOVERY;
			mpssas_discovery_end(sassc);
		}
		break;
	}
	/*
	 * XNU: no enclosure mapping (MPI2_EVENT_SAS_ENCL_DEVICE_STATUS_CHANGE)
	 * and no IR events: IT firmware, no mps_mapping.c.
	 */
	case MPI2_EVENT_LOG_ENTRY_ADDED:
	{
		pMpi2EventDataLogEntryAdded_t	logEntry;
		uint16_t			logQualifier;
		uint8_t				logCode;

		logEntry = (pMpi2EventDataLogEntryAdded_t)fw_event->event_data;
		logQualifier = logEntry->LogEntryQualifier;

		if (logQualifier == MPI2_WD_LOG_ENTRY) {
			logCode = logEntry->LogData[0];

			switch (logCode) {
			case MPI2_WD_SSD_THROTTLING:
				printf("WarpDrive Warning: IO Throttling has "
				    "occurred in the WarpDrive subsystem. "
				    "Check WarpDrive documentation for "
				    "additional details\n");
				break;
			case MPI2_WD_DRIVE_LIFE_WARN:
				printf("WarpDrive Warning: Program/Erase "
				    "Cycles for the WarpDrive subsystem in "
				    "degraded range. Check WarpDrive "
				    "documentation for additional details\n");
				break;
			case MPI2_WD_DRIVE_LIFE_DEAD:
				printf("WarpDrive Fatal Error: There are no "
				    "Program/Erase Cycles for the WarpDrive "
				    "subsystem. The storage device will be in "
				    "read-only mode. Check WarpDrive "
				    "documentation for additional details\n");
				break;
			case MPI2_WD_RAIL_MON_FAIL:
				printf("WarpDrive Fatal Error: The Backup Rail "
				    "Monitor has failed on the WarpDrive "
				    "subsystem. Check WarpDrive documentation "
				    "for additional details\n");
				break;
			default:
				break;
			}
		}
		break;
	}
	/*
	 * XNU: pseudo events queued by mps_xnu_periodic, run here so that no
	 * two config page reads overlap: a device add that failed on our side,
	 * the IOC temperature.
	 */
	case MPS_XNU_EVENT_READD:
	{
		u16 handle = *(u16 *)fw_event->event_data;
		u8 linkrate;

		switch (mps_xnu_readd_begin(sc, handle, &linkrate)) {
		case 1:
			(void)mpssas_add_device(sc, handle, linkrate);
			mps_xnu_readd_end(sc, handle);
			break;
		case MPS_XNU_READD_REIDENT:
			mps_xnu_reidentify(sc, handle);
			break;
		}
		break;
	}
	case MPS_XNU_EVENT_TEMP:
		mps_xnu_temperature(sc);
		break;
	case MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE:
	case MPI2_EVENT_SAS_BROADCAST_PRIMITIVE:
	default:
		mps_dprint(sc, MPS_TRACE,"Unhandled event 0x%0X\n",
		    fw_event->event);
		break;
	}
	mps_dprint(sc, evlvl, "(%d)->(%s) Event Free: [%x]\n",event_count,__func__, fw_event->event);
	mpssas_fw_event_free(sc, fw_event);
}

void
mpssas_firmware_event_work(void *arg, int pending)
{
	struct mps_fw_event_work *fw_event;
	struct mps_softc *sc;

	sc = (struct mps_softc *)arg;
	mps_lock(sc);
	while ((fw_event = TAILQ_FIRST(&sc->sassc->ev_queue)) != NULL) {
		TAILQ_REMOVE(&sc->sassc->ev_queue, fw_event, ev_link);
		mpssas_fw_work(sc, fw_event);
	}
	mps_unlock(sc);
}

static int
mpssas_add_device(struct mps_softc *sc, u16 handle, u8 linkrate){
	char devstring[80];
	struct mpssas_softc *sassc;
	struct mpssas_target *targ;
	Mpi2ConfigReply_t mpi_reply;
	Mpi2SasDevicePage0_t config_page;
	uint64_t sas_address;
	uint64_t parent_sas_address = 0;
	u32 device_info, parent_devinfo = 0;
	unsigned int id;
	int ret = 1, error = 0, i;
	struct mpssas_lun *lun;
	u8 is_SATA_SSD = 0;
	struct mps_command *cm;
	int unverified = 0;	/* XNU: D8 */

	sassc = sc->sassc;
	mpssas_startup_increment(sassc);
	/* XNU: retried in-line on local failures (mps_xnu.c) */
	if ((error = mps_xnu_get_sas_device_pg0(sc, &mpi_reply, &config_page,
	    MPI2_SAS_DEVICE_PGAD_FORM_HANDLE, handle)) != 0) {
		mps_dprint(sc, MPS_INFO|MPS_MAPPING|MPS_FAULT,
		    "Error reading SAS device %#x page0, iocstatus= 0x%x\n",
		    handle, mpi_reply.IOCStatus);
		sc->xnu.stats.config_failures++;	/* XNU */
		/* XNU: tried again later when the failure was ours */
		mps_xnu_readd_schedule(sc, handle, linkrate, error, &mpi_reply);
		error = ENXIO;
		goto out;
	}

	device_info = le32toh(config_page.DeviceInfo);

	if (((device_info & MPI2_SAS_DEVICE_INFO_SMP_TARGET) == 0)
	 && (le16toh(config_page.ParentDevHandle) != 0)) {
		Mpi2ConfigReply_t tmp_mpi_reply;
		Mpi2SasDevicePage0_t parent_config_page;

		if (mps_xnu_get_sas_device_pg0(sc, &tmp_mpi_reply,	/* XNU */
		    &parent_config_page, MPI2_SAS_DEVICE_PGAD_FORM_HANDLE,
		    le16toh(config_page.ParentDevHandle)) != 0) {
			mps_dprint(sc, MPS_MAPPING|MPS_FAULT,
			    "Error reading parent SAS device %#x page0, "
			    "iocstatus= 0x%x\n",
			    le16toh(config_page.ParentDevHandle),
			    tmp_mpi_reply.IOCStatus);
		} else {
			parent_sas_address = parent_config_page.SASAddress.High;
			parent_sas_address = (parent_sas_address << 32) |
				parent_config_page.SASAddress.Low;
			parent_devinfo = le32toh(parent_config_page.DeviceInfo);
		}
	}
	/* TODO Check proper endianness */
	sas_address = config_page.SASAddress.High;
	sas_address = (sas_address << 32) | config_page.SASAddress.Low;
        mps_dprint(sc, MPS_MAPPING, "Handle 0x%04x SAS Address from SAS device "
            "page0 = %jx\n", handle, (uintmax_t)sas_address);

	/*
	 * Always get SATA Identify information because this is used to
	 * determine if Start/Stop Unit should be sent to the drive when the
	 * system is shutdown.
	 */
	if (device_info & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) {
		ret = mpssas_get_sas_address_for_sata_disk(sc, &sas_address,
		    handle, device_info, &is_SATA_SSD);
		if (ret) {
			mps_dprint(sc, MPS_MAPPING|MPS_ERROR,
			    "%s: failed to get disk type (SSD or HDD) for SATA "
			    "device with handle 0x%04x\n",
			    __func__, handle);
			/*
			 * XNU: the IOC makes the SATA SAS address up per PHY,
			 * so it does not tell disks apart: identity unknown
			 * (mps_xnu_get_tid treats it as a new disk), unless
			 * a reinit's rediscovery keeps it (D8: IO held).
			 */
			sc->xnu.stats.sata_identify_failures++;
			sas_address = mps_xnu_ident_keep(sc, handle);
			unverified = sas_address != 0;
		} else {
			mps_dprint(sc, MPS_MAPPING, "Handle 0x%04x SAS Address "
			    "from SATA device = %jx\n", handle,
			    (uintmax_t)sas_address);
		}
	}

	/*
	 * XNU: the target id comes from mps_xnu_get_tid() instead of
	 * mps_mapping.c, keyed like mps_mapping_get_tid on the device's
	 * identity (SAS address, IDENTIFY-derived for SATA).
	 */
	id = mps_xnu_get_tid(sc, handle, sas_address);
	if (id == MPS_MAP_BAD_ID) {
		mps_dprint(sc, MPS_INFO, "failure at %s:%d/%s()! "
		    "Could not get ID for device with handle 0x%04x\n",
		    __FILE__, __LINE__, __func__, handle);
		error = ENXIO;
		goto out;
	}
	mps_dprint(sc, MPS_MAPPING, "%s: Target ID for added device is %d.\n",
	    __func__, id);

	/*
	 * Only do the ID check and reuse check if the target is not from a
	 * RAID Component. For Physical Disks of a Volume, the ID will be reused
	 * when a volume is deleted because the mapping entry for the PD will
	 * still be in the mapping table. The ID check should not be done here
	 * either since this PD is already being used.
	 */
	targ = &sassc->targets[id];
	if (!(targ->flags & MPS_TARGET_FLAGS_RAID_COMPONENT)) {
		if (mpssas_check_id(sassc, id) != 0) {
			mps_dprint(sc, MPS_MAPPING|MPS_INFO,
			    "Excluding target id %d\n", id);
			error = ENXIO;
			goto out;
		}

		if (targ->handle != 0x0) {
			mps_dprint(sc, MPS_MAPPING, "Attempting to reuse "
			    "target id %d handle 0x%04x\n", id, targ->handle);
			error = ENXIO;
			goto out;
		}
	}

	targ->devinfo = device_info;
	targ->devname = le32toh(config_page.DeviceName.High);
	targ->devname = (targ->devname << 32) | 
	    le32toh(config_page.DeviceName.Low);
	targ->encl_handle = le16toh(config_page.EnclosureHandle);
	targ->encl_slot = le16toh(config_page.Slot);
	targ->handle = handle;
	targ->parent_handle = le16toh(config_page.ParentDevHandle);
	targ->sasaddr = mps_to_u64(&config_page.SASAddress);
	targ->parent_sasaddr = le64toh(parent_sas_address);
	targ->parent_devinfo = parent_devinfo;
	targ->tid = id;
	targ->linkrate = (linkrate>>4);
	targ->flags = 0;
	if (is_SATA_SSD) {
		targ->flags = MPS_TARGET_IS_SATA_SSD;
	}
	TAILQ_INIT(&targ->commands);
	TAILQ_INIT(&targ->timedout_commands);
	while(!SLIST_EMPTY(&targ->luns)) {
		lun = SLIST_FIRST(&targ->luns);
		SLIST_REMOVE_HEAD(&targ->luns, lun_link);
		free(lun, M_MPT2);
	}
	SLIST_INIT(&targ->luns);

	mps_describe_devinfo(targ->devinfo, devstring, 80);
	mps_dprint(sc, MPS_MAPPING, "Found device <%s> <%s> <0x%04x> <%d/%d>\n",
	    devstring, mps_describe_table(mps_linkrate_names, targ->linkrate),
	    targ->handle, targ->encl_handle, targ->encl_slot);

	mpssas_rescan_target(sc, targ);
	mps_dprint(sc, MPS_MAPPING, "Target id 0x%x added\n", targ->tid);
	mps_xnu_readd_cancel(sc, handle);	/* XNU: no later retry */
	mps_xnu_ident_added(sc, targ, unverified);	/* XNU: D8 */

	/*
	 * Check all commands to see if the SATA_ID_TIMEOUT flag has been set.
	 * If so, send a Target Reset TM to the target that was just created.
	 * An Abort Task TM should be used instead of a Target Reset, but that
	 * would be much more difficult because targets have not been fully
	 * discovered yet, and LUN's haven't been setup.  So, just reset the
	 * target instead of the LUN.  The commands should complete once the
	 * target has been reset.
	 */
	for (i = 1; i < sc->num_reqs; i++) {
		cm = &sc->commands[i];
		if (cm->cm_flags & MPS_CM_FLAGS_SATA_ID_TIMEOUT) {
			targ->timeouts++;
			cm->cm_flags |= MPS_CM_FLAGS_TIMEDOUT;

			if ((targ->tm = mpssas_alloc_tm(sc)) != NULL) {
				mps_dprint(sc, MPS_INFO, "%s: sending Target "
				    "Reset for stuck SATA identify command "
				    "(cm = %p)\n", __func__, cm);
				targ->tm->cm_targ = targ;
				mpssas_send_reset(sc, targ->tm,
				    MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET);
			} else {
				mps_dprint(sc, MPS_ERROR, "Failed to allocate "
				    "tm for Target Reset after SATA ID command "
				    "timed out (cm %p)\n", cm);
			}
			/*
			 * No need to check for more since the target is
			 * already being reset.
			 */
			break;
		}
	}
out:
	mpssas_startup_decrement(sassc);
	return (error);
}

int
mpssas_get_sas_address_for_sata_disk(struct mps_softc *sc,
    u64 *sas_address, u16 handle, u32 device_info, u8 *is_SATA_SSD)
{
	Mpi2SataPassthroughReply_t mpi_reply;
	int i, rc, try_count;
	u32 *bufferptr;
	union _sata_sas_address hash_address;
	struct _ata_identify_device_data ata_identify;
	u8 buffer[MPT2SAS_MN_LEN + MPT2SAS_SN_LEN];
	u32 ioc_status;
	u8 sas_status;

	memset(&ata_identify, 0, sizeof(ata_identify));
	memset(&mpi_reply, 0, sizeof(mpi_reply));	/* XNU: read on failure */
	try_count = 0;
	do {
		rc = mpssas_get_sata_identify(sc, handle, &mpi_reply,
		    (char *)&ata_identify, sizeof(ata_identify), device_info);
		try_count++;
		ioc_status = le16toh(mpi_reply.IOCStatus)
		    & MPI2_IOCSTATUS_MASK;
		sas_status = mpi_reply.SASStatus;
		switch (ioc_status) {
		case MPI2_IOCSTATUS_SUCCESS:
			break;
		case MPI2_IOCSTATUS_SCSI_PROTOCOL_ERROR:
			/* No sense sleeping.  this error won't get better */
			break;
		default:
			if (sc->spinup_wait_time > 0) {
				mps_dprint(sc, MPS_INFO, "Sleeping %d seconds "
				    "after SATA ID error to wait for spinup\n",
				    sc->spinup_wait_time);
				msleep(&sc->msleep_fake_chan, &sc->mps_mtx, 0,
				    "mpsid", sc->spinup_wait_time * hz);
			}
		}
		/* XNU: stopped, gone or asleep: no point in trying again */
		if (rc == ENXIO && (sc->mps_flags & (MPS_FLAGS_SHUTDOWN |
		    MPS_FLAGS_XNU_GONE | MPS_FLAGS_XNU_SUSPENDED)))
			break;
	} while (((rc && (rc != EWOULDBLOCK)) ||
	    	 (ioc_status && 
		  (ioc_status != MPI2_IOCSTATUS_SCSI_PROTOCOL_ERROR))
	       || sas_status) && (try_count < 5));

	if (rc == 0 && !ioc_status && !sas_status) {
		mps_dprint(sc, MPS_MAPPING, "%s: got SATA identify "
		    "successfully for handle = 0x%x with try_count = %d\n",
		    __func__, handle, try_count);
	} else {
		mps_dprint(sc, MPS_MAPPING, "%s: handle = 0x%x failed\n",
		    __func__, handle);
		return -1;
	}
	/* Copy & byteswap the 40 byte model number to a buffer */
	for (i = 0; i < MPT2SAS_MN_LEN; i += 2) {
		buffer[i] = ((u8 *)ata_identify.model_number)[i + 1];
		buffer[i + 1] = ((u8 *)ata_identify.model_number)[i];
	}
	/* Copy & byteswap the 20 byte serial number to a buffer */
	for (i = 0; i < MPT2SAS_SN_LEN; i += 2) {
		buffer[MPT2SAS_MN_LEN + i] =
		    ((u8 *)ata_identify.serial_number)[i + 1];
		buffer[MPT2SAS_MN_LEN + i + 1] =
		    ((u8 *)ata_identify.serial_number)[i];
	}
	bufferptr = (u32 *)buffer;
	/* There are 60 bytes to hash down to 8. 60 isn't divisible by 8,
	 * so loop through the first 56 bytes (7*8),
	 * and then add in the last dword.
	 */
	hash_address.word.low  = 0;
	hash_address.word.high = 0;
	for (i = 0; (i < ((MPT2SAS_MN_LEN+MPT2SAS_SN_LEN)/8)); i++) {
		hash_address.word.low += *bufferptr;
		bufferptr++;
		hash_address.word.high += *bufferptr;
		bufferptr++;
	}
	/* Add the last dword */
	hash_address.word.low += *bufferptr;
	/* Make sure the hash doesn't start with 5, because it could clash
	 * with a SAS address. Change 5 to a D.
	 */
	if ((hash_address.word.high & 0x000000F0) == (0x00000050))
		hash_address.word.high |= 0x00000080;
	*sas_address = (u64)hash_address.wwid[0] << 56 |
	    (u64)hash_address.wwid[1] << 48 | (u64)hash_address.wwid[2] << 40 |
	    (u64)hash_address.wwid[3] << 32 | (u64)hash_address.wwid[4] << 24 |
	    (u64)hash_address.wwid[5] << 16 | (u64)hash_address.wwid[6] <<  8 |
	    (u64)hash_address.wwid[7];
	if (ata_identify.rotational_speed == 1) {
		*is_SATA_SSD = 1;
	}

	return 0;
}

static int
mpssas_get_sata_identify(struct mps_softc *sc, u16 handle,
    Mpi2SataPassthroughReply_t *mpi_reply, char *id_buffer, int sz, u32 devinfo)
{
	Mpi2SataPassthroughRequest_t *mpi_request;
	Mpi2SataPassthroughReply_t *reply = NULL;
	struct mps_command *cm;
	char *buffer;
	int error = 0;

	buffer = malloc( sz, M_MPT2, M_NOWAIT | M_ZERO);
	if (!buffer)
		return ENOMEM;

	if ((cm = mps_alloc_command(sc)) == NULL) {
		free(buffer, M_MPT2);
		return (EBUSY);
	}
	mpi_request = (MPI2_SATA_PASSTHROUGH_REQUEST *)cm->cm_req;
	bzero(mpi_request,sizeof(MPI2_SATA_PASSTHROUGH_REQUEST));
	mpi_request->Function = MPI2_FUNCTION_SATA_PASSTHROUGH;
	mpi_request->VF_ID = 0;
	mpi_request->DevHandle = htole16(handle);
	mpi_request->PassthroughFlags = (MPI2_SATA_PT_REQ_PT_FLAGS_PIO |
	    MPI2_SATA_PT_REQ_PT_FLAGS_READ);
	mpi_request->DataLength = htole32(sz);
	mpi_request->CommandFIS[0] = 0x27;
	mpi_request->CommandFIS[1] = 0x80;
	mpi_request->CommandFIS[2] =  (devinfo &
	    MPI2_SAS_DEVICE_INFO_ATAPI_DEVICE) ? 0xA1 : 0xEC;
	cm->cm_sge = &mpi_request->SGL;
	cm->cm_sglsize = sizeof(MPI2_SGE_IO_UNION);
	cm->cm_flags = MPS_CM_FLAGS_SGE_SIMPLE | MPS_CM_FLAGS_DATAIN;
	cm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	cm->cm_length = htole32(sz);
	cm->cm_data = buffer;

	/*
	 * Use a custom handler to avoid reinit'ing the controller on timeout.
	 * This fixes a problem where the FW does not send a reply sometimes
	 * when a bad disk is in the topology. So, this is used to timeout the
	 * command so that processing can continue normally.
	 */
	cm->cm_timeout_handler = mpssas_ata_id_timeout;

	error = mps_wait_command(sc, &cm, MPS_ATA_ID_TIMEOUT, CAN_SLEEP);

	/* mpssas_ata_id_timeout does not reset controller */
	KASSERT(cm != NULL, ("%s: surprise command freed", __func__));

	reply = (Mpi2SataPassthroughReply_t *)cm->cm_reply;
	if (error || (reply == NULL)) {
		/* FIXME */
 		/*
 		 * If the request returns an error then we need to do a diag
 		 * reset
 		 */ 
 		mps_dprint(sc, MPS_INFO|MPS_FAULT|MPS_MAPPING,
		    "Request for SATA PASSTHROUGH page completed with error %d\n",
		    error);
		error = ENXIO;
		goto out;
	}
	bcopy(buffer, id_buffer, sz);
	bcopy(reply, mpi_reply, sizeof(Mpi2SataPassthroughReply_t));
	if ((le16toh(reply->IOCStatus) & MPI2_IOCSTATUS_MASK) !=
	    MPI2_IOCSTATUS_SUCCESS) {
		mps_dprint(sc, MPS_INFO|MPS_MAPPING|MPS_FAULT,
		    "Error reading device %#x SATA PASSTHRU; iocstatus= 0x%x\n",
		    handle, reply->IOCStatus);
		error = ENXIO;
		goto out;
	}
out:
	/*
	 * If the SATA_ID_TIMEOUT flag has been set for this command, don't free
	 * it.  The command and buffer will be freed after we send a Target
	 * Reset TM and the command comes back from the controller.
	 */
	if ((cm->cm_flags & MPS_CM_FLAGS_SATA_ID_TIMEOUT) == 0) {
		mps_free_command(sc, cm);
		free(buffer, M_MPT2);
	}
	return (error);
}

/*
 * This is completion handler to make sure that commands and allocated
 * buffers get freed when timed out SATA ID commands finally complete after
 * we've reset the target.  In the normal case, we wait for the command to
 * complete.
 */
static void
mpssas_ata_id_complete(struct mps_softc *sc, struct mps_command *cm)
{
	mps_dprint(sc, MPS_INFO, "%s ATA ID completed late cm %p sc %p\n",
	    __func__, cm, sc);

	free(cm->cm_data, M_MPT2);
	mps_free_command(sc, cm);
}


static void
mpssas_ata_id_timeout(struct mps_softc *sc, struct mps_command *cm)
{
	mps_dprint(sc, MPS_INFO, "%s ATA ID command timeout cm %p sc %p\n",
	    __func__, cm, sc);

	/*
	 * The Abort Task cannot be sent from here because the driver has not
	 * completed setting up targets.  Instead, the command is flagged so
	 * that special handling will be used to send a target reset.
	 */
	cm->cm_flags |= MPS_CM_FLAGS_SATA_ID_TIMEOUT;

	/*
	 * Since we will no longer be waiting for the command to complete,
	 * set a completion handler to make sure we free all resources.
	 */
	cm->cm_complete = mpssas_ata_id_complete;
}
