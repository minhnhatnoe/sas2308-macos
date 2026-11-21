/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_xnu_os.h - the interface between the IOKit class (SAS2308HBA.cpp) and
 * the FreeBSD-shaped core, implemented in mps_xnu_os.cpp.
 *
 * mps_xnu_os.cpp is the only kext translation unit that includes the core
 * headers (core/compat/queue.h cannot share a translation unit with the
 * <sys/queue.h> that IOService.h pulls in). It implements the mps_os_*()
 * platform hooks on xnu and the mps_kext_*() calls below, which take and
 * return plain types only. The softc stays opaque to the IOKit side.
 *
 * Locking: every mps_kext_*() call takes mps_mtx itself; none may be called
 * with it held, i.e. not from the host callbacks marked "mps_mtx held".
 */
#ifndef MPS_XNU_OS_H
#define MPS_XNU_OS_H

#include <stddef.h>
#include <stdint.h>

#include "mps_xnu_stats.h"	/* struct mps_xnu_stats (core, plain C) */

#ifdef __cplusplus
extern "C" {
#endif

/* mps_dprint classes and log levels (mpsvar.h, mps_xnu.h; checked in mps_xnu_os.cpp) */
#define MPSK_INFO		(1u << 0)
#define MPSK_FAULT		(1u << 1)
#define MPSK_EVENT		(1u << 2)
#define MPSK_RECOVERY		(1u << 4)
#define MPSK_ERROR		(1u << 5)
#define MPSK_INIT		(1u << 6)
#define MPSK_XINFO		(1u << 7)
#define MPSK_USER		(1u << 8)
#define MPSK_MAPPING		(1u << 9)
#define MPSK_DEBUG_ALL		((1u << 11) - 1)
#define MPSK_LEVEL_INFO		2	/* default */
#define MPSK_LEVEL_TRACE	4
#define MPSK_MAX_TARGETS	64	/* MPS_XNU_MAX_TARGETS: target ids 0..63 */

struct mps_kext;			/* softc + device_t + glue state */

/* Filled by the kext before mps_kext_create(); lives as long as the kext object. */
struct mps_kext_host {
	void		*hba;		/* SAS2308HBA */
	void		*mapper;	/* IOMapper * (the DART), NULL = physical */
	volatile uint8_t *regs;		/* BAR1 mapping (MPI2 system interface) */
	uint32_t	 unit;
	volatile int	 use_iolog;	/* lines to IOLog instead of os_log */
	/* platform events (mps_pci.c counterparts); mps_mtx may be held */
	int	(*setup_interrupts)(void *hba);
	void	(*free_interrupts)(void *hba);
	int	(*pci_restore)(void *hba);
	void	(*controller_dead)(void *hba);
	int	(*pci_present)(void *hba);	/* vendor id reads back (D11) */
	/* a tunable (mps_os_getenv, D33): boot-arg, else personality property */
	int	(*getenv)(void *hba, const char *name, char *buf, size_t len);
	/* completions were queued by a callout or the reinit task (mps_mtx held) */
	void	(*io_done_kick)(void *hba);
	/*
	 * CAM async events (mps_mtx held). MPSK_ASYNC_SCAN: for
	 * every target id in `tids', bit = 1 if mps_xnu_target_present() at
	 * the time of the event; gens[id] = the generation of the device
	 * behind the id (mps_xnu_target_gen: changes with its identity).
	 */
	void	(*async)(void *hba, int event, uint64_t tids, uint64_t present,
		    const uint32_t *gens);
};

enum {
	MPSK_ASYNC_SCAN = 1,		/* AC_XNU_SCAN (xpt_rescan) */
	MPSK_ASYNC_BUS_RESET,		/* AC_BUS_RESET (IOC reinit) */
	MPSK_ASYNC_SENT_BDR		/* AC_SENT_BDR (target reset) */
};

/* ---- life cycle (mock_ioc_test.cpp Rig order) -------------------------- */

struct mps_kext	*mps_kext_create(struct mps_kext_host *host, uint16_t devid);
/* mps_get_tunables, parent DMA tag, mps_attach, async registration */
int	 mps_kext_attach(struct mps_kext *k, uint32_t debug_mask);
/* config_intrhook: mps_startup (unmask, port enable) */
void	 mps_kext_start(struct mps_kext *k);
/* stop path: no more TM waits; returns when every TM waiter has left */
void	 mps_kext_stop_waiters(struct mps_kext *k);
/* mps_free (MUR, every ccb completed once) + parent DMA tag; attached only */
void	 mps_kext_detach(struct mps_kext *k);
/* after detach or a failed attach; the done queue must be drained */
void	 mps_kext_destroy(struct mps_kext *k);
uint32_t mps_kext_max_segs(struct mps_kext *k);		/* per IO */
uint32_t mps_kext_max_tasks(struct mps_kext *k);	/* ReportMaximumTaskCount, from IOCFacts */

/*
 * The IOCFacts limits and what mps_resize_queues made of them (the kext's
 * "Controller Limits" property); 0 if not attached. Memory: what the core
 * allocated for these sizes (DMA: bus_dmamem_alloc sizes before page
 * rounding; heap: commands, chain trackers, command DMA maps).
 */
struct mps_kext_limits {
	/* IOCFacts (MPI2_IOC_FACTS_REPLY) */
	uint16_t	request_credit;
	uint16_t	high_priority_credit;
	uint16_t	max_reply_post_depth;	/* MaxReplyDescriptorPostQueueDepth */
	uint8_t		max_chain_depth;
	uint8_t		max_msix_vectors;
	uint32_t	req_frame_bytes;	/* IOCRequestFrameSize * 4 */
	uint32_t	reply_frame_bytes;	/* ReplyFrameSize * 4 */
	uint16_t	max_targets;
	uint16_t	max_volumes;
	/* effective (mps_resize_queues) */
	uint32_t	req_frames;		/* normal request frames (num_reqs - num_prireqs) */
	uint32_t	pri_req_frames;		/* high-priority (task management) frames */
	uint32_t	reply_frames;		/* num_replies */
	uint32_t	post_queue_depth;	/* pqdepth */
	uint32_t	chain_frames;		/* num_chains */
	uint32_t	os_commands;		/* xnu.io_cmds */
	uint32_t	max_segs;		/* buffer_dmat nsegments */
	uint64_t	maxio;			/* sc->maxio: largest IO, bytes (mps.max_io_pages) */
	uint64_t	dma_bytes;
	uint64_t	heap_bytes;
};
int	 mps_kext_limits(struct mps_kext *k, struct mps_kext_limits *l);	/* 0 = filled */

/* zeroed heap memory for the glue (M_WAITOK context: may block); NULL = none */
void	*mps_kext_zalloc(size_t size);
void	 mps_kext_zfree(void *ptr, size_t size);
uint64_t mps_kext_max_seg_bytes(void);			/* one SGE */

/* ---- interrupts -------------------------------------------------------- */

void	 mps_kext_intr(struct mps_kext *k);		/* mps_intr_msi */

/* ---- SCSI IO: an XPT_SCSI_IO ccb in the task's HBA data ---------------- */

struct mps_kext_seg {			/* = bus_dma_segment_t */
	uint64_t	addr;
	uint64_t	len;
};

size_t	 mps_kext_task_size(struct mps_kext *k);	/* HBA data per task (after attach) */
struct mps_kext_seg *mps_kext_task_segs(void *td);

enum { MPSK_DIR_NONE, MPSK_DIR_IN, MPSK_DIR_OUT };
enum {
	MPSK_IO_STATUS,			/* the device answered: scsi_status */
	MPSK_IO_GONE,			/* no such device / controller gone */
	MPSK_IO_RETRY,			/* transport failure (reset, abort) */
	MPSK_IO_TIMEOUT,		/* terminated after the OS timeout */
	MPSK_IO_REJECT			/* invalid request */
};
struct mps_kext_io_result {
	int		 kind;		/* MPSK_IO_* */
	uint32_t	 cam_status;	/* for log lines */
	uint8_t		 scsi_status;
	uint64_t	 transferred;
	const uint8_t	*sense;		/* in td, valid until the task completes */
	uint32_t	 sense_len;
};

enum { MPSK_SUBMIT_POSTED, MPSK_SUBMIT_HELD, MPSK_SUBMIT_DONE };
/*
 * xpt_action with mps_mtx held. DONE: completed synchronously (*res, for
 * the log); the task is on the done queue like any other completion, so
 * the family sees it through CompleteParallelTask. timeout_ms: the
 * task's own timeout; the core arms its callout 10 s after it, a
 * backstop should the family's timer be lost (0: none). gen: the
 * generation of the nub the task came from; another device behind tid
 * now (mps_xnu_target_gen differs): DONE, CAM_DEV_NOT_THERE.
 */
int	 mps_kext_submit(struct mps_kext *k, void *td, void *os_task,
	    uint64_t os_tag, uint32_t tid, uint64_t lun, const uint8_t *cdb,
	    uint8_t cdblen,
	    int dir, uint32_t nsegs, uint64_t len, uint32_t timeout_ms,
	    uint32_t gen, struct mps_kext_io_result *res);
#define	MPSK_BACKSTOP_MS	10000	/* core timeout = task timeout + this */
#define	MPSK_UNTIMED_BACKSTOP_MS 600000	/* core timeout of a task with none (D15) */
/* pop one completed task (NULL: none); usable until mps_kext_destroy */
void	*mps_kext_next_done(struct mps_kext *k, struct mps_kext_io_result *res);
/*
 * The OS timer of a task fired. D4: FreeBSD times a ccb from its post to
 * the IOC (callout_reset_sbt in mpssas_action_scsiio); the family timer was
 * armed at submit. A task still held, or posted less than timeout_ms ago,
 * is not timed out: *rearm_ms (>= 1) is what is left, for
 * SetTimeoutForTask. Otherwise mps_xnu_ccb_timeout, *rearm_ms = 0.
 */
int	 mps_kext_timeout(struct mps_kext *k, void *td, void *os_task,
	    uint32_t timeout_ms, uint32_t *rearm_ms);

/* ---- debug triggers for hardware tests (sas2308ctl) --------------------- */
enum { MPSK_DEBUG_REINIT = 1, MPSK_DEBUG_HOLD, MPSK_DEBUG_LOSE };
int	 mps_kext_debug(struct mps_kext *k, int what, uint32_t arg);

/* ---- OS task management (D10): FreeBSD's paths, waits for the end ------ */

enum { MPSK_TM_COMPLETE, MPSK_TM_REJECTED, MPSK_TM_FAILED };
/*
 * ABORT_TASK (abort_*: the task as submitted; one already completed is
 * "complete"): the task's timeout recovery, returns when it is back; LUN
 * and TARGET RESET: XPT_RESET_DEV (a target reset); anything else
 * REJECTED. why: a reason for the log.
 */
int	 mps_kext_tm(struct mps_kext *k, uint8_t type, uint32_t tid,
	    uint64_t lun, void *abort_td, void *abort_task, uint64_t abort_tag,
	    char *why, size_t whylen);

/*
 * Termination: read the doorbell now, so a surprise removal (all-ones) is
 * noticed before the targets are torn down. Returns 0 if the controller is
 * gone or disabled. Noticing only queues the reinit that fails the IO;
 * mps_kext_disable_now fails it here.
 */
int	 mps_kext_check_present(struct mps_kext *k);
/*
 * A gone controller: wait for a running reinit (it stops at its next
 * wait), then mps_xnu_disable() now: every posted, held and TM ccb is
 * completed CAM_DEV_NOT_THERE before this returns (FreeBSD: mps_free in
 * detach completes every ccb before the SIM goes). No-op if present.
 */
void	 mps_kext_disable_now(struct mps_kext *k);

/* ---- system sleep / shutdown (mps_xnu_suspend, mps_xnu_resume) ---------- */

/*
 * hold new IO, drain up to drain_ms, MUR, no register access until resume;
 * shutdown: fail held and new IO instead, short bounded stop, no resume
 */
int	 mps_kext_suspend(struct mps_kext *k, uint32_t drain_ms, int shutdown);
/* reinit on the core's taskqueue; IO held until the rediscovery is done */
int	 mps_kext_resume(struct mps_kext *k);

/* ---- locks and time for the IOKit-free glue (SAS2308Glue) -------------- */

/* the core's mps_os_lock_* hooks: IOLock on xnu, the host model in tests */
void	*mps_kext_lock_alloc(const char *name);
void	 mps_kext_lock_free(void *lock);
void	 mps_kext_lock(void *lock);
void	 mps_kext_unlock(void *lock);
void	 mps_kext_sleep(void *lock, void *chan);	/* drops `lock' while asleep */
void	 mps_kext_wakeup(void *chan);
/* once per kext load (mps_xnu_os.cpp): logs, the wakeup lock; 0 = ok */
int	 mps_kext_os_init(void);

/* ---- targets, statistics, logging -------------------------------------- */

int	 mps_kext_target_present(struct mps_kext *k, uint32_t tid);
/*
 * Before a target's nub is destroyed: fail the target's held IO
 * (CAM_DEV_NOT_THERE) and wait until its posted IO has come back (the
 * removal's target reset terminates it), the controller is disabled, or
 * the kext stops; posted IO is never completed here (the IOC may still
 * DMA). After reinit_ms a controller reinit is requested, and the wait
 * goes on. Returns what is left (only after a stop).
 */
int	 mps_kext_target_drain(struct mps_kext *k, uint32_t tid, uint32_t reinit_ms);
/* host tests only: the softc, for the model's interrupt delivery */
void	*mps_kext_softc(struct mps_kext *k);

/* SAS Device Page 0 as read by mpssas_add_device (0: no such target) */
struct mps_kext_target_info {
	uint64_t	sas_address;	/* host order */
	uint32_t	device_info;	/* MPI2_SAS_DEVICE_INFO_* */
	uint8_t		link_rate;	/* MPI2_SAS_NEG_LINK_RATE_* */
};
int	 mps_kext_target_info(struct mps_kext *k, uint32_t tid,
	    struct mps_kext_target_info *info);
#define	MPSK_DEVINFO_SSP_TARGET	0x00000400u	/* MPI2_SAS_DEVICE_INFO_SSP_TARGET */
#define	MPSK_DEVINFO_STP_TARGET	0x00000200u	/* MPI2_SAS_DEVICE_INFO_STP_TARGET */
#define	MPSK_DEVINFO_SATA_DEVICE 0x00000080u	/* MPI2_SAS_DEVICE_INFO_SATA_DEVICE */

/* IOCFacts as read at attach: for the Controller Characteristics */
struct mps_kext_controller_info {
	char		fw_version[16];	/* "20.00.07.00" (sc->fw_version) */
	char		msg_version[8];	/* MPI "2.0" */
	uint16_t	product_id;	/* IOCFacts.ProductID */
	uint8_t		num_ports;	/* IOCFacts.NumberOfPorts */
	uint8_t		max_link_rate;	/* best negotiated rate seen, 0 = none */
};
void	 mps_kext_controller_info(struct mps_kext *k,
	    struct mps_kext_controller_info *info);

struct mps_kext_snapshot {
	struct mps_xnu_stats st;
	uint32_t	held;		/* ccbs held by the thin CAM now */
	uint32_t	tms_busy;	/* high-priority SMIDs in use */
	int		gone;
	int		resetting;
};
void	 mps_kext_snapshot(struct mps_kext *k, struct mps_kext_snapshot *s);
void	 mps_kext_reset_stats(struct mps_kext *k);
void	 mps_kext_set_debug(struct mps_kext *k, uint32_t mask);

uint32_t	 mps_kext_level_mask(int level);
const char	*mps_kext_level_name(int level);
int		 mps_kext_parse_level(const char *s);
/* FreeBSD mps_parse_debug on `mask' ("info,fault", "+trace", "0x7ff") */
void		 mps_kext_parse_debug(const char *s, uint32_t *mask);
void		 mps_kext_describe_debug(uint32_t mask, char *buf, size_t len);
const char	*mps_kext_tm_type_str(uint8_t type);
/* one line to os_log / IOLog, as mps_dprint does */
void		 mps_kext_log(const struct mps_kext_host *host, uint32_t cls,
		    const char *line);

#ifdef __cplusplus
}
#endif

#endif /* MPS_XNU_OS_H */
