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

static u32 event_count;
static void mpssas_fw_work(struct mps_softc *sc,
    struct mps_fw_event_work *fw_event);
static void mpssas_fw_event_free(struct mps_softc *,
    struct mps_fw_event_work *);
static int mpssas_add_device(struct mps_softc *sc, u16 handle, u8 linkrate);

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
	sassc = sc->sassc;

	mps_dprint(sc, MPS_EVENT, "(%d)->(%s) Working on  Event: [%x]\n",
			event_count++,__func__,fw_event->event);
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
	case MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE:
	case MPI2_EVENT_SAS_BROADCAST_PRIMITIVE:
	default:
		mps_dprint(sc, MPS_TRACE,"Unhandled event 0x%0X\n",
		    fw_event->event);
		break;
	}
	mps_dprint(sc, MPS_EVENT, "(%d)->(%s) Event Free: [%x]\n",event_count,__func__, fw_event->event);
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
	int error = 0;
	struct mpssas_lun *lun;
	u8 is_SATA_SSD = 0;

	sassc = sc->sassc;
	mpssas_startup_increment(sassc);
	if (mps_config_get_sas_device_pg0(sc, &mpi_reply, &config_page,
	    MPI2_SAS_DEVICE_PGAD_FORM_HANDLE, handle) != 0) {
		mps_dprint(sc, MPS_INFO|MPS_MAPPING|MPS_FAULT,
		    "Error reading SAS device %#x page0, iocstatus= 0x%x\n",
		    handle, mpi_reply.IOCStatus);
		sc->xnu.stats.config_failures++;	/* XNU */
		error = ENXIO;
		goto out;
	}

	device_info = le32toh(config_page.DeviceInfo);

	if (((device_info & MPI2_SAS_DEVICE_INFO_SMP_TARGET) == 0)
	 && (le16toh(config_page.ParentDevHandle) != 0)) {
		Mpi2ConfigReply_t tmp_mpi_reply;
		Mpi2SasDevicePage0_t parent_config_page;

		if (mps_config_get_sas_device_pg0(sc, &tmp_mpi_reply,
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
	 * XNU: no SATA identify (mpssas_get_sas_address_for_sata_disk), so a
	 * SATA disk keeps the SAS address the IOC made up for it and is not
	 * told apart as an SSD.  The target id comes from mps_xnu_get_tid()
	 * (firmware handle based) instead of mps_mapping.c.
	 */
	id = mps_xnu_get_tid(sc, handle);
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

	/* XNU: no SATA identify, so no MPS_CM_FLAGS_SATA_ID_TIMEOUT commands */
out:
	mpssas_startup_decrement(sassc);
	return (error);
}
