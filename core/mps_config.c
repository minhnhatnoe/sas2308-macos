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
#include "mps_mpi.h"
#include "mpsvar.h"
/* TODO Move headers to mpsvar */

/*
 * XNU: only mps_config_get_sas_device_pg0() is ported (mpssas_add_device);
 * the other config page readers (IOC page 8, manufacturing page 10, DPM,
 * BIOS page 3, RAID volume/physical disk pages, WarpDrive) belong to
 * mps_mapping.c, IR and WarpDrive support, which are not ported.
 */

/**
 * mps_config_get_sas_device_pg0 - obtain sas device page 0
 * @sc: per adapter object
 * @mpi_reply: reply mf payload returned from firmware
 * @config_page: contents of the config page
 * @form: GET_NEXT_HANDLE or HANDLE
 * @handle: device handle
 * Context: sleep.
 *
 * Returns 0 for success, non-zero for failure.
 */
int
mps_config_get_sas_device_pg0(struct mps_softc *sc, Mpi2ConfigReply_t
    *mpi_reply, Mpi2SasDevicePage0_t *config_page, u32 form, u16 handle)
{
	MPI2_CONFIG_REQUEST *request;
	MPI2_CONFIG_REPLY *reply = NULL;
	struct mps_command *cm;
	Mpi2SasDevicePage0_t *page = NULL;
	int error = 0;
	u16 ioc_status;

	mps_dprint(sc, MPS_TRACE, "%s\n", __func__);

	if ((cm = mps_alloc_command(sc)) == NULL) {
		printf("%s: command alloc failed @ line %d\n", __func__,
		    __LINE__);
		error = EBUSY;
		goto out;
	}
	request = (MPI2_CONFIG_REQUEST *)cm->cm_req;
	bzero(request, sizeof(MPI2_CONFIG_REQUEST));
	request->Function = MPI2_FUNCTION_CONFIG;
	request->Action = MPI2_CONFIG_ACTION_PAGE_HEADER;
	request->Header.PageType = MPI2_CONFIG_PAGETYPE_EXTENDED;
	request->ExtPageType = MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE;
	request->Header.PageNumber = 0;
	request->Header.PageVersion = MPI2_SASDEVICE0_PAGEVERSION;
	cm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	cm->cm_data = NULL;
	error = mps_wait_command(sc, &cm, 60, CAN_SLEEP);
	if (cm != NULL)
		reply = (MPI2_CONFIG_REPLY *)cm->cm_reply;
	if (error || (reply == NULL)) {
		/* FIXME */
		/*
		 * If the request returns an error then we need to do a diag
		 * reset
		 */ 
		printf("%s: request for header completed with error %d\n",
		    __func__, error);
		error = ENXIO;
		goto out;
	}
	ioc_status = le16toh(reply->IOCStatus) & MPI2_IOCSTATUS_MASK;
	bcopy(reply, mpi_reply, sizeof(MPI2_CONFIG_REPLY));
	if (ioc_status != MPI2_IOCSTATUS_SUCCESS) {
		/* FIXME */
		/*
		 * If the request returns an error then we need to do a diag
		 * reset
		 */ 
		printf("%s: header read with error; iocstatus = 0x%x\n",
		    __func__, ioc_status);
		error = ENXIO;
		goto out;
	}
	/* We have to do free and alloc for the reply-free and reply-post
	 * counters to match - Need to review the reply FIFO handling.
	 */
	mps_free_command(sc, cm);

	if ((cm = mps_alloc_command(sc)) == NULL) {
		printf("%s: command alloc failed @ line %d\n", __func__,
		    __LINE__);
		error = EBUSY;
		goto out;
	}
	request = (MPI2_CONFIG_REQUEST *)cm->cm_req;
	bzero(request, sizeof(MPI2_CONFIG_REQUEST));
	request->Function = MPI2_FUNCTION_CONFIG;
	request->Action = MPI2_CONFIG_ACTION_PAGE_READ_CURRENT;
	request->Header.PageType = MPI2_CONFIG_PAGETYPE_EXTENDED;
	request->ExtPageType = MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE;
	request->Header.PageNumber = 0;
	request->Header.PageVersion = MPI2_SASDEVICE0_PAGEVERSION;
	request->ExtPageLength = mpi_reply->ExtPageLength;
	request->PageAddress = htole32(form | handle);
	cm->cm_length = le16toh(mpi_reply->ExtPageLength) * 4;
	cm->cm_sge = &request->PageBufferSGE;
	cm->cm_sglsize = sizeof(MPI2_SGE_IO_UNION);
	cm->cm_flags = MPS_CM_FLAGS_SGE_SIMPLE | MPS_CM_FLAGS_DATAIN;
	cm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	/*
	 * XNU: bus_dmamap_load() maps bus_dmamem_alloc() memory only, so the
	 * page is read into the preallocated sc->xnu.cfg_page (lent to the
	 * command's map) instead of a malloc() buffer.
	 */
	page = mps_xnu_config_page(sc, cm);
	if (!page) {
		printf("%s: page alloc failed\n", __func__);
		error = ENOMEM;
		goto out;
	}
	cm->cm_data = page;

	error = mps_wait_command(sc, &cm, 60, CAN_SLEEP);
	if (cm != NULL)
		reply = (MPI2_CONFIG_REPLY *)cm->cm_reply;
	if (error || (reply == NULL)) {
		/* FIXME */
		/*
		 * If the request returns an error then we need to do a diag
		 * reset
		 */ 
		printf("%s: request for page completed with error %d\n",
		    __func__, error);
		error = ENXIO;
		goto out;
	}
	ioc_status = le16toh(reply->IOCStatus) & MPI2_IOCSTATUS_MASK;
	bcopy(reply, mpi_reply, sizeof(MPI2_CONFIG_REPLY));
	if (ioc_status != MPI2_IOCSTATUS_SUCCESS) {
		/* FIXME */
		/*
		 * If the request returns an error then we need to do a diag
		 * reset
		 */ 
		printf("%s: page read with error; iocstatus = 0x%x\n",
		    __func__, ioc_status);
		error = ENXIO;
		goto out;
	}
	bcopy(page, config_page, MIN(cm->cm_length, 
	    sizeof(Mpi2SasDevicePage0_t)));
out:
	mps_xnu_config_page_done(sc, page);	/* XNU: instead of free() */
	if (cm) {
		bus_dmamap_unload(sc->buffer_dmat, cm->cm_dmamap);	/* XNU */
		mps_free_command(sc, cm);
	}
	return (error);
}
