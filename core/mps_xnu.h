/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * mps_xnu.h - additions with no counterpart in FreeBSD sys/dev/mps:
 * log-level presets on top of the mps_dprint classes, %b replacement,
 * CDB/sense/ASC/loginfo decoders (FreeBSD gets these from CAM), the
 * failed-IO rate limiter, statistics, simple target-id mapping (FreeBSD's
 * mps_mapping.c needs config pages), surprise removal, event acks, and the
 * driver-specific platform hooks.
 */
#ifndef _MPS_XNU_H_
#define _MPS_XNU_H_

#include "compat/mps_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mps_softc;

/* ---- tunable defaults (mps_get_tunables) ------------------------------- */

#define MPS_XNU_IO_CMDS		64	/* SCSI IOs the OS may have outstanding */
#define MPS_XNU_INTERNAL_CMDS	8	/* events, acks, port enable, removal */
#define MPS_XNU_REQ_FRAMES	(MPS_XNU_IO_CMDS + MPS_XNU_INTERNAL_CMDS)
#define MPS_XNU_PRI_REQ_FRAMES	4	/* task management */
#define MPS_XNU_EVT_REPLY_FRAMES 32
#define MPS_XNU_REPLY_FRAMES	(MPS_XNU_REQ_FRAMES + MPS_XNU_PRI_REQ_FRAMES)
#define MPS_XNU_MAXIO_PAGES	256	/* 1 MiB */
#define MPS_XNU_REINIT_TRIES	3	/* mps_reinit attempts on nested faults */
#define MPS_XNU_MAX_TARGETS	64	/* target ids handed out */
#define MPS_XNU_CFG_TRIES	3	/* page 0 attempts in a row (10, 20 ms apart) */
#define MPS_XNU_READD_SLOTS	16	/* devices waiting for a later add */
#define MPS_XNU_READD_TRIES	8	/* later attempts, 1 s .. 5 s apart */
#define MPS_XNU_TEMP_PERIOD_S	30	/* IOC temperature poll */
#define MPS_XNU_TEMP_WARN_C	85	/* warning above, */
#define MPS_XNU_TEMP_REARM_C	80	/* re-armed below */

/* ---- log levels: presets of the mps_dprint class mask ------------------ */

#define MPS_LEVEL_ERROR		0	/* fault, error */
#define MPS_LEVEL_WARN		1	/* + recovery */
#define MPS_LEVEL_INFO		2	/* + info, init, event, mapping (default) */
#define MPS_LEVEL_DEBUG		3	/* + xinfo, log, user */
#define MPS_LEVEL_TRACE		4	/* + trace */
#define MPS_DEBUG_ALL		((1u << 11) - 1)

uint32_t	 mps_xnu_level_mask(int level);
const char	*mps_xnu_level_name(int level);
int		 mps_xnu_parse_level(const char *s);
/* os_log category of a line: its most specific class */
const char	*mps_xnu_category(uint32_t cls);
void		 mps_xnu_describe_debug(uint32_t mask, char *buf, size_t len);

/* ---- the kernel printf %b conversion -------------------------------------- */

const char	*mps_xnu_bits(char *buf, size_t len, uint32_t v, const char *bits);
/* printf("%b", v, bits) -> printf("%s", MPS_XNU_BITS(v, bits)); C only */
#define MPS_XNU_BITS(v, bits)	mps_xnu_bits((char[160]){ 0 }, 160, (v), (bits))

/* ---- decoders (FreeBSD: CAM scsi_all.c) ------------------------------------- */

const char	*mps_xnu_ioc_state_str(uint32_t doorbell);
const char	*mps_xnu_tm_type_str(uint8_t type);
const char	*mps_xnu_sense_key_str(uint8_t key);
const char	*mps_xnu_cdb_str(uint8_t opcode);
const char	*mps_xnu_asc_str(uint8_t asc, uint8_t ascq);	/* NULL: unknown */
int		 mps_xnu_parse_sense(const uint8_t *sense, uint32_t len,
		    uint8_t *key, uint8_t *asc, uint8_t *ascq);
void		 mps_xnu_loginfo_str(uint32_t loginfo, char *buf, size_t len);
void		 mps_xnu_lun_str(const uint8_t lun[8], char *buf, size_t len);
int		 mps_xnu_cdb_lba(const uint8_t *cdb, uint8_t cdblen,
		    uint64_t *lba, uint32_t *blocks);

/* ---- failed-IO rate limiter --------------------------------------------- */

#define MPS_XNU_RL_SLOTS	8	/* per target: handle % 8 */
#define MPS_XNU_RL_BURST	5	/* lines per window */
#define MPS_XNU_RL_WINDOW_MS	10000

struct mps_xnu_ratelimit {
	uint64_t	window_start_ms;
	uint32_t	in_window;
	uint32_t	suppressed;
};

/* 1: log the line (*suppressed: lines dropped in the previous window) */
int	mps_xnu_ratelimit_allow(struct mps_xnu_ratelimit *rl, uint64_t now_ms,
	    uint32_t burst, uint32_t window_ms, uint32_t *suppressed);
/* 1: the window closed with lines suppressed; print the summary */
int	mps_xnu_ratelimit_flush(struct mps_xnu_ratelimit *rl, uint64_t now_ms,
	    uint32_t window_ms, uint32_t *suppressed);

/* ---- statistics (M4 MpsStats): mps_xnu_stats.h ------------------------- */

#include "mps_xnu_stats.h"

/* ---- per-controller XNU state: struct mps_softc member `xnu' ------------- */

struct mps_xnu {
	struct mps_xnu_stats	 stats;
	struct mps_xnu_ratelimit ioerr_rl[MPS_XNU_RL_SLOTS];
	uint16_t		 ioerr_handle[MPS_XNU_RL_SLOTS];
	uint16_t		 ioerr_tid[MPS_XNU_RL_SLOTS];
	int			 reinit_pending;	/* mps_reinit_task queued */
	int			 disabled;		/* mps_xnu_disable() ran */
	uint16_t		 tid_handle[MPS_XNU_MAX_TARGETS]; /* last handle */
	uint64_t		 reinit_start_us;
	int			 gone_logged;
	/* config pages (mps_config.c): one DMA buffer, one read at a time */
	bus_dma_tag_t		 cfg_dmat;
	bus_dmamap_t		 cfg_map;
	void			*cfg_page;
	int			 cfg_busy;
	/* system sleep / shutdown: new IO held, OS TMs refused (mps.c) */
	int			 suspending;
	int			 suspended_logged;
	/* SAS Device Page 0 retries: in-line backoff, then later re-adds */
	int			 cfg_retry;		/* msleep channel */
	struct mps_xnu_readd {
		uint16_t	 handle;		/* 0: free slot */
		uint8_t		 linkrate;
		uint8_t		 tries;			/* deferred attempts made */
		int		 queued;		/* on the event queue */
		uint64_t	 due_us;
	}			 readd[MPS_XNU_READD_SLOTS];
	/* IOC temperature (IO Unit Page 7), read on the event queue */
	int			 temp_queued;
	int			 temp_absent;		/* units NOT_PRESENT: stop */
	int			 temp_warned;		/* above the warning, not re-armed */
	int			 temp_logged;		/* first reading logged */
	uint64_t		 temp_last_us;
};

#define	MPS_XNU_CFG_PAGE_BYTES	1024	/* largest config page read */

/*
 * Pseudo events on the firmware event queue (mpssas_fw_work), so that they
 * run one at a time with discovery: never two config page reads at once.
 */
#define	MPS_XNU_EVENT_READD	0xFF01	/* retry mpssas_add_device(handle) */
#define	MPS_XNU_EVENT_TEMP	0xFF02	/* read IO Unit Page 7 */
int	mpssas_xnu_queue_work(struct mps_softc *sc, uint16_t event,
	    const void *data, size_t len);	/* mps_sas_lsi.c, mps_mtx held */

/* mps_config_get_sas_device_pg0 with in-line retries for local failures */
int	mps_xnu_get_sas_device_pg0(struct mps_softc *sc, void *mpi_reply,
	    void *config_page, uint32_t form, uint16_t handle);
/* page 0 of a device being added failed: retry later or give up (logged) */
void	mps_xnu_readd_schedule(struct mps_softc *sc, uint16_t handle,
	    uint8_t linkrate, int error, const void *mpi_reply);
/* forget a pending re-add (handle 0: all) */
void	mps_xnu_readd_cancel(struct mps_softc *sc, uint16_t handle);
/*
 * MPS_XNU_EVENT_READD on the event queue: 1 if the add is to be tried
 * (*linkrate set); then mps_xnu_readd_end after mpssas_add_device
 */
int	mps_xnu_readd_begin(struct mps_softc *sc, uint16_t handle,
	    uint8_t *linkrate);
void	mps_xnu_readd_end(struct mps_softc *sc, uint16_t handle);
/* MPS_XNU_EVENT_TEMP on the event queue */
void	mps_xnu_temperature(struct mps_softc *sc);

/* target id for a newly announced device handle (MPS_MAP_BAD_ID: none) */
u_int	mps_xnu_get_tid(struct mps_softc *sc, uint16_t handle);
/* 1 if target `tid' is attached and not being removed (*handle set) */
int	mps_xnu_target_present(struct mps_softc *sc, u_int tid,
	    uint16_t *handle);
/* all-ones register read: the controller is gone (surprise removal) */
void	mps_xnu_gone(struct mps_softc *sc, uint32_t offset);
/* EVENT_ACK for an event with AckRequired set */
void	mps_xnu_event_ack(struct mps_softc *sc, void *event_reply);
/* every reinit attempt failed: disable the controller (FreeBSD panics) */
void	mps_xnu_disable(struct mps_softc *sc, int error);
/* copy sc->chain_* / held counters into sc->xnu.stats */
void	mps_xnu_stats_update(struct mps_softc *sc);
/* queue sc->mps_reinit_task (mps_reinit sleeps: never called inline) */
void	mps_xnu_request_reinit(struct mps_softc *sc, const char *why);
/* M2-M4: doorbell FAULT check when an interrupt finds the queue empty */
void	mps_xnu_check_fault(struct mps_softc *sc);
/* 1 s from mps_periodic: rate-limit summaries, statistics */
void	mps_xnu_periodic(struct mps_softc *sc);
/* SCSI IO posted / completed: trace lines, statistics, failed-IO line */
struct mps_command;
union ccb;
void	mps_xnu_io_start(struct mps_softc *sc, struct mps_command *cm);
void	mps_xnu_io_done(struct mps_softc *sc, struct mps_command *cm,
	    union ccb *ccb, void *scsi_io_reply);
/* a TM reply arrived (reply != NULL): recovery line, statistics */
void	mps_xnu_tm_done(struct mps_softc *sc, struct mps_command *tm);
/* a reinit finished: duration statistics */
void	mps_xnu_reinit_done(struct mps_softc *sc);
/* register access while MPS_FLAGS_XNU_SUSPENDED: refused, logged once, 0 */
uint32_t mps_xnu_suspended_access(struct mps_softc *sc, uint32_t offset);
/*
 * mps_config.c: the DMA buffer for a config page read of cm->cm_length
 * bytes, lent to cm's map (NULL: busy or too large); _done when finished
 */
void	*mps_xnu_config_page(struct mps_softc *sc, struct mps_command *cm);
void	 mps_xnu_config_page_done(struct mps_softc *sc, void *page);
void	 mps_xnu_config_free(struct mps_softc *sc);	/* mps_free */
/* SAS Device Page 0 of target `tid' as read by mpssas_add_device (0: none) */
int	 mps_xnu_target_info(struct mps_softc *sc, u_int tid, uint64_t *sasaddr,
	    uint32_t *devinfo, uint8_t *linkrate);

/* ---- driver-specific platform hooks (mps_pci.c counterparts) ------------- */

int	mps_os_setup_interrupts(device_t dev);	/* after the queues exist */
void	mps_os_free_interrupts(device_t dev);
int	mps_os_pci_restore(device_t dev);	/* after a diag reset */
void	mps_os_controller_dead(device_t dev);	/* stop bus mastering */

#ifdef __cplusplus
}
#endif

#endif /* !_MPS_XNU_H_ */
