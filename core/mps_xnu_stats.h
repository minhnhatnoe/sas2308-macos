/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_xnu_stats.h - the driver statistics (sc->xnu.stats).  Only <stdint.h>, so
 * that the kext's IOKit translation unit (which cannot include the compat
 * headers, see docs/design.md) can read a snapshot of them.
 */
#ifndef _MPS_XNU_STATS_H_
#define _MPS_XNU_STATS_H_

#include <stdint.h>

/* ---- statistics (as the pre-FreeBSD driver's MpsStats) ------------------- */

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
	/* chain pool (from sc->chain_alloc_fail / chain_free_lowwater) */
	uint32_t	chain_alloc_fail;
	uint32_t	chain_free_low;
	/* system sleep / shutdown (mps_xnu_suspend) */
	uint32_t	suspends, resumes;
	uint32_t	suspend_stop_failed;	/* the IOC did not stop, suspended anyway (D49) */
	uint32_t	suspend_drain_left;	/* commands still owned by the IOC at the MUR */
	uint32_t	suspended_access;	/* register accesses refused while suspended */
	uint32_t	halt_capped;		/* commands whose time left the halt capped (F1) */
	uint32_t	halt_stops;		/* reinits turned into the shutdown stop (F1) */
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
	uint32_t	temp_timeouts;		/* read not answered: polling stopped until a reinit (D31) */
	uint32_t	ioc_temp_valid;		/* ioc_temp_c holds a reading */
	int32_t		ioc_temp_c;		/* last reading, degrees C */
	/* review fixes */
	uint32_t	reset_requeues;		/* IO re-sent after our reset */
	uint32_t	reinit_coalesced;	/* reinit requests covered by a running one */
	uint32_t	removals_queued;	/* removals that waited for a free TM */
	uint32_t	identity_changes;	/* a different disk behind a target id */
	uint32_t	sata_identify_failures;
	/* FreeBSD deviation fixes */
	uint32_t	ua_retries;		/* UNIT ATTENTION 29/xx after our reset, re-sent (D3) */
	uint32_t	transient_allones;	/* all-ones register read, controller still there (D11) */
	uint32_t	timer_rearms;		/* family timer re-armed: held, or IOC owned it less long (D4) */
	uint32_t	resume_ready;		/* wakes by the READY path: no diag reset (D12) */
	uint32_t	resume_diag_reset;	/* wakes that needed the diag reset */
	uint32_t	kassert_hits;		/* KASSERT failures and panic() calls survived, all controllers (D55) */
	/* batch G2 */
	uint32_t	ordered_tags;		/* SIMPLE tasks sent ORDERED: da's periodic tag (D46) */
	/* S1 */
	uint32_t	idle_stops;		/* idle START STOP UNIT (START 0) completed here, not sent */
	uint32_t	idle_stops_sent;	/* ... sent: the disk not in use, sas2308_idle_spindown 2 (S2) */
	uint32_t	idle_stops_grace;	/* ... of the completed here: within sas2308_spindown_grace (S2) */
};

#endif /* !_MPS_XNU_STATS_H_ */
