/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * mps_xnu_stats.h - the M4 statistics (sc->xnu.stats).  Only <stdint.h>, so
 * that the kext's IOKit translation unit (which cannot include the compat
 * headers, see docs/design.md) can read a snapshot of them.
 */
#ifndef _MPS_XNU_STATS_H_
#define _MPS_XNU_STATS_H_

#include <stdint.h>

/* ---- statistics (M4 MpsStats) -------------------------------------------- */

struct mps_xnu_stats {
	/* IO */
	uint64_t	ios;		/* SCSI IOs completed (any status) */
	uint64_t	read_bytes, write_bytes;
	uint64_t	lat_sum_us;	/* post -> completion */
	uint32_t	lat_max_us;
	uint32_t	held;		/* IOs requeued/held during recovery */
	uint32_t	held_timeouts;
	/* errors by class */
	uint32_t	err_check_condition;
	uint32_t	err_scsi_status;
	uint32_t	err_terminated;
	uint32_t	err_device_gone;
	uint32_t	err_transport;
	uint32_t	io_timeouts;
	/* task management */
	uint32_t	tm_sent[16];	/* by MPI2 TaskType */
	uint32_t	tm_timeouts;
	uint32_t	tm_late_replies;
	uint32_t	tm_failed;
	uint32_t	aborts, lun_resets, target_resets;
	uint32_t	recoveries;
	uint32_t	recovered_abort, recovered_lun_reset,
			recovered_target_reset;
	/* controller */
	uint32_t	faults;
	uint32_t	diag_resets;
	uint32_t	reinits;
	uint32_t	reinit_failures;
	uint32_t	reinit_max_ms;
	/* replies / events */
	uint32_t	stale_replies;	/* reply for an idle SMID */
	uint32_t	bad_replies;	/* malformed descriptor dropped */
	uint32_t	events;
	uint32_t	event_ack_dropped;
	/* chain pool (from sc->chain_alloc_fail / chain_free_lowwater) */
	uint32_t	chain_alloc_fail;
	uint32_t	chain_free_low;
	/* system sleep / shutdown (mps_xnu_suspend) */
	uint32_t	suspends, resumes;
	uint32_t	suspend_drain_left;	/* commands still owned by the IOC at the MUR */
	uint32_t	suspended_access;	/* register accesses refused while suspended */
	/* SAS Device Page 0 reads (mpssas_add_device) */
	uint32_t	config_reads, config_failures;
	uint32_t	config_retries;		/* in-line retries (no command, buffer busy) */
	uint32_t	readd_scheduled;	/* device adds retried later */
	uint32_t	readd_given_up;
	/*
	 * CHECK CONDITION with sense key RECOVERED ERROR (not an error; e.g.
	 * SAT's ATA PASS-THROUGH status return): not in err_check_condition
	 */
	uint32_t	recovered_sense;
	/* IOC temperature (IO Unit Page 7, mps_xnu_temperature) */
	uint32_t	temp_reads, temp_failures;
	uint32_t	temp_warnings;		/* crossings of MPS_XNU_TEMP_WARN_C */
	uint32_t	ioc_temp_valid;		/* ioc_temp_c holds a reading */
	int32_t		ioc_temp_c;		/* last reading, degrees C */
};

#endif /* !_MPS_XNU_STATS_H_ */
