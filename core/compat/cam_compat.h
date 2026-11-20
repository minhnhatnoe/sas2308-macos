/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * cam_compat.h - thin CAM for the ported mps_sas.c: the ccb layout, status
 * codes and xpt_*() calls the driver uses, on top of mps_compat.h.  Values
 * and field names follow FreeBSD cam/cam.h, cam/cam_ccb.h and
 * cam/scsi/scsi_all.h so that the driver code stays verbatim.
 *
 * Semantics (docs/design.md "Compat layer contract"):
 * - The platform glue (or the test) submits a ccb with xpt_action(), mps_mtx
 *   held.  A ccb that arrives while the SIM queue or its device queue is
 *   frozen is held, in arrival order, and handed to the SIM's action routine
 *   when the queues are released.
 * - xpt_done() with CAM_REQUEUE_REQ holds the ccb again (in its original
 *   arrival order); any other status calls ccb_h.cbfcnp(NULL, ccb), with
 *   mps_mtx held.  CAM_DEV_QFRZN on a completed ccb releases that freeze
 *   (the periph's job in FreeBSD) and is cleared; CAM_RELEASE_SIMQ releases
 *   the SIM queue.
 * - xpt_async() and xpt_rescan() call the callbacks registered with
 *   xpt_register_async() for the bus (rescans as AC_XNU_SCAN with the scan
 *   ccb as `arg'), with mps_mtx held.
 */
#ifndef _MPS_CAM_COMPAT_H_
#define _MPS_CAM_COMPAT_H_

#include "mps_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef u_int		path_id_t;
typedef u_int		target_id_t;
typedef uint64_t	lun_id_t;
typedef uint32_t	cam_status;

#define CAM_XPT_PATH_ID		((path_id_t)~0)
#define CAM_BUS_WILDCARD	((path_id_t)~0)
#define CAM_TARGET_WILDCARD	((target_id_t)~0)
#define CAM_LUN_WILDCARD	(~(u_int)0)

/* cam_status (cam.h) */
#define CAM_REQ_INPROG		0x00
#define CAM_REQ_CMP		0x01
#define CAM_REQ_ABORTED		0x02
#define CAM_UA_ABORT		0x03
#define CAM_REQ_CMP_ERR		0x04
#define CAM_BUSY		0x05
#define CAM_REQ_INVALID		0x06
#define CAM_PATH_INVALID	0x07
#define CAM_DEV_NOT_THERE	0x08
#define CAM_UA_TERMIO		0x09
#define CAM_SEL_TIMEOUT		0x0a
#define CAM_CMD_TIMEOUT		0x0b
#define CAM_SCSI_STATUS_ERROR	0x0c
#define CAM_MSG_REJECT_REC	0x0d
#define CAM_SCSI_BUS_RESET	0x0e
#define CAM_UNCOR_PARITY	0x0f
#define CAM_AUTOSENSE_FAIL	0x10
#define CAM_NO_HBA		0x11
#define CAM_DATA_RUN_ERR	0x12
#define CAM_UNEXP_BUSFREE	0x13
#define CAM_SEQUENCE_FAIL	0x14
#define CAM_CCB_LEN_ERR		0x15
#define CAM_PROVIDE_FAIL	0x16
#define CAM_BDR_SENT		0x17
#define CAM_REQ_TERMIO		0x18
#define CAM_UNREC_HBA_ERROR	0x19
#define CAM_REQ_TOO_BIG		0x1a
#define CAM_REQUEUE_REQ		0x1b
#define CAM_ATA_STATUS_ERROR	0x1c
#define CAM_SCSI_IT_NEXUS_LOST	0x1d
#define CAM_SMP_STATUS_ERROR	0x1e
#define CAM_REQ_SOFTTIMEOUT	0x1f
#define CAM_RESRC_UNAVAIL	0x34
#define CAM_LUN_INVALID		0x38
#define CAM_TID_INVALID		0x39
#define CAM_FUNC_NOTAVAIL	0x3a
#define CAM_STATUS_MASK		0x3F
#define CAM_DEV_QFRZN		0x40
#define CAM_AUTOSNS_VALID	0x80
#define CAM_RELEASE_SIMQ	0x100
#define CAM_SIM_QUEUED		0x200
#define CAM_QOS_VALID		0x400

/* ccb_h.flags (cam_ccb.h) */
#define CAM_CDB_POINTER		0x00000001
#define CAM_DIR_BOTH		0x00000000
#define CAM_DIR_IN		0x00000040
#define CAM_DIR_OUT		0x00000080
#define CAM_DIR_NONE		0x000000C0
#define CAM_DIR_MASK		0x000000C0
#define CAM_DATA_VADDR		0x00000000
#define CAM_DATA_PADDR		0x00000010
#define CAM_DATA_SG		0x00040000
#define CAM_DATA_SG_PADDR	0x00040010
#define CAM_DATA_BIO		0x00200000
#define CAM_DATA_MASK		0x00240010
#define CAM_SCATTER_VALID	CAM_DATA_SG
#define CAM_DIS_AUTOSENSE	0x00000020
#define CAM_TAG_ACTION_NONE	0x00

/* tag_action (scsi_message.h) */
#define MSG_SIMPLE_Q_TAG	0x20
#define MSG_HEAD_OF_Q_TAG	0x21
#define MSG_ORDERED_Q_TAG	0x22
#define MSG_ACA_TASK		0x24

/* func_code */
typedef enum {
	XPT_NOOP		= 0x00,
	XPT_SCSI_IO		= 0x01,
	XPT_ABORT		= 0x10,
	XPT_RESET_BUS		= 0x11,
	XPT_RESET_DEV		= 0x12,
	XPT_TERM_IO		= 0x13,
	XPT_SCAN_BUS		= 0x14,
	XPT_SCAN_LUN		= 0x16,
	XPT_SMP_IO		= 0x1b,
	XPT_SCAN_TGT		= 0x1E
} xpt_opcode;

/* async codes (cam_ccb.h ac_code) */
#define AC_BUS_RESET		0x00000001
#define AC_SENT_BDR		0x00000010
#define AC_FOUND_DEVICE		0x00000080
#define AC_LOST_DEVICE		0x00000100
#define AC_ADVINFO_CHANGED	0x00008000
/* XNU: xpt_rescan() -> async callbacks; arg is the XPT_SCAN_* ccb */
#define AC_XNU_SCAN		0x40000000

#define CAM_PRIORITY_NORMAL	5
#define SCSI_STATUS_OK		0x00	/* scsi_all.h */
#define IOCDBLEN		16
#define SSD_FULL_SIZE		252
#define SSD_KEY_RECOVERED_ERROR	0x01	/* scsi_all.h */
#define SSD_KEY_MEDIUM_ERROR	0x03
#define SSD_KEY_HARDWARE_ERROR	0x04

struct scsi_sense_data {
	uint8_t		error_code;
	uint8_t		sense_buf[SSD_FULL_SIZE - 1];
};

struct cam_periph;
struct cam_sim;
union ccb;

struct cam_path {
	path_id_t	path_id;
	target_id_t	target_id;
	lun_id_t	lun_id;
};

typedef union {
	void		*ptr;
	u_long		field;
	uint8_t		bytes[sizeof(uintptr_t)];
} ccb_priv_entry;

typedef union {
	ccb_priv_entry	entries[2];
	uint8_t		bytes[2 * sizeof(ccb_priv_entry)];
} ccb_ppriv_area, ccb_spriv_area;

typedef union {
	uint64_t	sim_data;
} ccb_qos_area;

struct ccb_hdr {
	TAILQ_ENTRY(ccb_hdr) sim_links;	/* XNU: held queue link */
	uint64_t	xnu_seq;	/* XNU: arrival order (xpt_action) */
	uint64_t	xnu_start_us;	/* XNU: arrival time (latency incl. hold) */
	int		xnu_held;	/* XNU: on the held queue now */
	int		xnu_was_held;	/* XNU: held at least once */
	int		xnu_resets;	/* XNU: re-sent after our own resets */
	xpt_opcode	func_code;
	uint32_t	status;
	struct cam_path	*path;
	path_id_t	path_id;
	target_id_t	target_id;
	lun_id_t	target_lun;
	uint32_t	flags;
	uint32_t	xflags;
	ccb_ppriv_area	periph_priv;
	ccb_spriv_area	sim_priv;
	ccb_qos_area	qos;
	uint32_t	timeout;	/* ms */
	uint32_t	pinfo_priority;
	void		(*cbfcnp)(struct cam_periph *, union ccb *);
};
#define ppriv_ptr0	periph_priv.entries[0].ptr
#define ppriv_ptr1	periph_priv.entries[1].ptr
#define spriv_ptr0	sim_priv.entries[0].ptr
#define spriv_ptr1	sim_priv.entries[1].ptr
#define spriv_field0	sim_priv.entries[0].field
#define spriv_field1	sim_priv.entries[1].field

typedef union {
	uint8_t		*cdb_ptr;
	uint8_t		cdb_bytes[IOCDBLEN];
} cdb_t;

struct ccb_scsiio {
	struct ccb_hdr	ccb_h;
	union ccb	*next_ccb;
	uint8_t		*req_map;
	uint8_t		*data_ptr;	/* CAM_DATA_SG_PADDR: bus_dma_segment_t[] */
	uint32_t	dxfer_len;
	struct scsi_sense_data sense_data;
	uint8_t		sense_len;
	uint8_t		cdb_len;
	uint16_t	sglist_cnt;
	uint8_t		scsi_status;
	uint8_t		sense_resid;
	uint32_t	resid;
	cdb_t		cdb_io;
	uint8_t		*msg_ptr;
	uint16_t	msg_len;
	uint8_t		tag_action;
	uint8_t		priority;
	u_int		tag_id;
	u_int		init_id;
};

union ccb {
	struct ccb_hdr		ccb_h;
	struct ccb_scsiio	csio;
};

/* ---- devq / sim ------------------------------------------------------------ */

struct cam_devq {
	int		max_openings;
};

/* A frozen device queue (target, lun; CAM_LUN_WILDCARD = whole target). */
struct cam_xnu_frozen {
	TAILQ_ENTRY(cam_xnu_frozen) link;
	target_id_t	target_id;
	lun_id_t	lun_id;
	int		count;
};

struct cam_xnu_async {
	TAILQ_ENTRY(cam_xnu_async) link;
	uint32_t	events;
	void		(*callback)(void *, uint32_t, struct cam_path *, void *);
	void		*arg;
};

typedef void sim_action_func(struct cam_sim *sim, union ccb *ccb);
typedef void sim_poll_func(struct cam_sim *sim);

struct cam_sim {
	sim_action_func		*sim_action;
	sim_poll_func		*sim_poll;
	const char		*sim_name;
	void			*softc;
	struct mtx		*mtx;
	path_id_t		 path_id;
	u_int			 unit_number;
	u_int			 bus_id;
	int			 max_tagged_dev_openings;
	int			 max_dev_openings;
	struct cam_devq		*devq;
	/* XNU: the thin CAM state */
	int			 freeze;		/* xpt_freeze_simq count */
	uint64_t		 seq;			/* last arrival number */
	int			 running;		/* in cam_xnu_run_held() */
	int			 rerun;
	uint32_t		 held_total;		/* ccbs ever held */
	TAILQ_HEAD(cam_xnu_ccbq, ccb_hdr) held;			/* by arrival order */
	TAILQ_HEAD(, cam_xnu_frozen) frozen;
	TAILQ_HEAD(, cam_xnu_async) async;
};

struct cam_devq	*cam_simq_alloc(uint32_t max_sim_transactions);
void		 cam_simq_free(struct cam_devq *devq);
struct cam_sim	*cam_sim_alloc(sim_action_func sim_action,
		    sim_poll_func sim_poll, const char *sim_name, void *softc,
		    uint32_t unit, struct mtx *mtx, int max_dev_transactions,
		    int max_tagged_dev_transactions, struct cam_devq *queue);
void		 cam_sim_free(struct cam_sim *sim, int free_devq);
#define cam_sim_path(sim)	((sim)->path_id)
#define cam_sim_softc(sim)	((sim)->softc)
#define cam_sim_name(sim)	((sim)->sim_name)
#define cam_sim_unit(sim)	((sim)->unit_number)
#define cam_sim_bus(sim)	((sim)->bus_id)

int		 xpt_bus_register(struct cam_sim *sim, device_t parent,
		    uint32_t bus);
int		 xpt_bus_deregister(path_id_t path_id);
struct cam_sim	*cam_xnu_sim_for_path(path_id_t path_id);

#define xpt_periph	((struct cam_periph *)NULL)
cam_status	 xpt_create_path(struct cam_path **new_path_ptr,
		    struct cam_periph *perph, path_id_t path_id,
		    target_id_t target_id, lun_id_t lun_id);
void		 xpt_free_path(struct cam_path *path);
#define xpt_path_path_id(p)	((p)->path_id)
#define xpt_path_target_id(p)	((p)->target_id)
#define xpt_path_lun_id(p)	((p)->lun_id)
void		 xpt_setup_ccb(struct ccb_hdr *ccb_h, struct cam_path *path,
		    uint32_t priority);

union ccb	*xpt_alloc_ccb(void);
union ccb	*xpt_alloc_ccb_nowait(void);
void		 xpt_free_ccb(union ccb *ccb);

void		 xpt_action(union ccb *ccb);
int		 bus_dmamap_load_ccb(bus_dma_tag_t dmat, bus_dmamap_t map,
		    union ccb *ccb, bus_dmamap_callback_t *callback,
		    void *callback_arg, int flags);
void		 xpt_done(union ccb *ccb);
uint32_t	 xpt_freeze_simq(struct cam_sim *sim, u_int count);
void		 xpt_release_simq(struct cam_sim *sim, int run_queue);
uint32_t	 xpt_freeze_devq(struct cam_path *path, u_int count);
void		 xpt_release_devq(struct cam_path *path, u_int count,
		    int run_queue);
#define xpt_hold_boot()		do { } while (0)
#define xpt_release_boot()	do { } while (0)
void		 xpt_rescan(union ccb *ccb);
void		 xpt_async(uint32_t async_code, struct cam_path *path,
		    void *async_arg);
cam_status	 xpt_register_async(int event, void (*cbfunc)(void *,
		    uint32_t, struct cam_path *, void *), void *cbarg,
		    struct cam_path *path);

/* "(mps0:0:<target>:<lun>): " and "<CDB name>. CDB: xx xx ... " */
void		 xpt_path_sbuf(struct cam_path *path, struct sbuf *sb);
void		 scsi_command_string(struct ccb_scsiio *csio, struct sbuf *sb);

/* XNU: number of ccbs held by the thin CAM (frozen queues / requeue). */
int		 cam_xnu_held_count(struct cam_sim *sim);
/* XNU: complete every held ccb with `status' (detach, controller gone). */
void		 cam_xnu_flush_held(struct cam_sim *sim, uint32_t status);
int		 cam_xnu_flush_held_target(struct cam_sim *sim, target_id_t tid,
		    uint32_t status);
/*
 * XNU: if `ccb' is held, take it off the held queue and complete it with
 * `status' (OS timeout or abort of a task that never reached the SIM).
 * Returns 1 if it was held (and is now completed), 0 if not.
 */
int		 cam_xnu_cancel_held(struct cam_sim *sim, union ccb *ccb,
		    uint32_t status);

/* ---- scsi_all.h byte helpers ---------------------------------------------- */

static __inline void
scsi_ulto2b(uint32_t val, uint8_t *bytes)
{
	bytes[0] = (val >> 8) & 0xff;
	bytes[1] = val & 0xff;
}

static __inline void
scsi_ulto3b(uint32_t val, uint8_t *bytes)
{
	bytes[0] = (val >> 16) & 0xff;
	bytes[1] = (val >> 8) & 0xff;
	bytes[2] = val & 0xff;
}

static __inline void
scsi_ulto4b(uint32_t val, uint8_t *bytes)
{
	bytes[0] = (val >> 24) & 0xff;
	bytes[1] = (val >> 16) & 0xff;
	bytes[2] = (val >> 8) & 0xff;
	bytes[3] = val & 0xff;
}

static __inline uint32_t
scsi_2btoul(const uint8_t *bytes)
{
	return ((bytes[0] << 8) | bytes[1]);
}

static __inline uint32_t
scsi_3btoul(const uint8_t *bytes)
{
	return ((bytes[0] << 16) | (bytes[1] << 8) | bytes[2]);
}

static __inline uint32_t
scsi_4btoul(const uint8_t *bytes)
{
	return (((uint32_t)bytes[0] << 24) | (bytes[1] << 16) |
	    (bytes[2] << 8) | bytes[3]);
}

static __inline uint64_t
scsi_8btou64(const uint8_t *bytes)
{
	return (((uint64_t)scsi_4btoul(bytes) << 32) |
	    scsi_4btoul(bytes + 4));
}

#ifdef __cplusplus
}
#endif

#endif /* !_MPS_CAM_COMPAT_H_ */
