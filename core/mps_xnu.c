/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 * Decoder tables derived from FreeBSD sys/cam/scsi/scsi_all.c:
 * Copyright (c) 1997, 1998, 1999 Justin T. Gibbs.
 * Copyright (c) 1997, 1998, 2003 Kenneth D. Merry.
 * All rights reserved.
 *
 * mps_xnu.c - see mps_xnu.h.  The decoder tables follow CAM's scsi_all.c
 * (scsi_op_desc, scsi_sense_key_text, asc_table) in content, not in code.
 */
#include "compat/mps_compat.h"
#include "compat/cam_compat.h"
#include "mps_mpi.h"
#include "mpsvar.h"
#include "mps_sas.h"
#include "mps_table.h"

/* ---- log levels ---------------------------------------------------------- */

static const struct {
	const char	*name;
	uint32_t	 flag;
} mps_xnu_classes[] = {
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

uint32_t
mps_xnu_level_mask(int level)
{
	uint32_t m = MPS_FAULT | MPS_ERROR;

	if (level >= MPS_LEVEL_WARN)
		m |= MPS_RECOVERY;
	if (level >= MPS_LEVEL_INFO)
		m |= MPS_INFO | MPS_INIT | MPS_EVENT | MPS_MAPPING;
	if (level >= MPS_LEVEL_DEBUG)
		m |= MPS_XINFO | MPS_LOG | MPS_USER;
	if (level >= MPS_LEVEL_TRACE)
		m |= MPS_TRACE;
	return (m);
}

const char *
mps_xnu_level_name(int level)
{
	static const char *const names[] = { "error", "warn", "info",
	    "debug", "trace" };

	return (level >= 0 && level <= MPS_LEVEL_TRACE ? names[level] : "?");
}

int
mps_xnu_parse_level(const char *s)
{
	int l;

	if (s == NULL || *s == '\0')
		return (-1);
	if (s[0] >= '0' && s[0] <= '0' + MPS_LEVEL_TRACE && s[1] == '\0')
		return (s[0] - '0');
	for (l = 0; l <= MPS_LEVEL_TRACE; l++)
		if (strcasecmp(s, mps_xnu_level_name(l)) == 0)
			return (l);
	if (strcasecmp(s, "warning") == 0)
		return (MPS_LEVEL_WARN);
	return (-1);
}

const char *
mps_xnu_category(uint32_t cls)
{
	/* Most specific first: MPS_RECOVERY|MPS_INFO is "recovery". */
	static const uint32_t order[] = { MPS_FAULT, MPS_RECOVERY, MPS_EVENT,
	    MPS_MAPPING, MPS_INIT, MPS_LOG, MPS_USER, MPS_ERROR, MPS_TRACE,
	    MPS_XINFO, MPS_INFO };
	size_t i, j;

	for (i = 0; i < nitems(order); i++) {
		if ((cls & order[i]) == 0)
			continue;
		for (j = 0; j < nitems(mps_xnu_classes); j++)
			if (mps_xnu_classes[j].flag == order[i])
				return (mps_xnu_classes[j].name);
	}
	return ("info");
}

/*
 * os_log type: errors/faults are ERROR; lines tagged with an info-level
 * class are DEFAULT (shown by `log stream`, persisted); XINFO/TRACE/LOG/USER
 * only lines are DEBUG (need `log stream --level debug`).  FreeBSD
 * printf/device_printf lines (class 0, e.g. a config page read failure) go
 * to the console: DEFAULT.
 */
int
mps_xnu_log_type(uint32_t cls)
{
	if (cls == 0)
		return (MPS_XNU_LOG_DEFAULT);
	if (cls & (MPS_FAULT | MPS_ERROR))
		return (MPS_XNU_LOG_ERROR);
	if (cls & (MPS_INFO | MPS_RECOVERY | MPS_INIT | MPS_EVENT | MPS_MAPPING))
		return (MPS_XNU_LOG_DEFAULT);
	return (MPS_XNU_LOG_DEBUG);
}

void
mps_xnu_describe_debug(uint32_t mask, char *buf, size_t len)
{
	size_t used = 0, i;
	int w;

	buf[0] = '\0';
	for (i = 0; i < nitems(mps_xnu_classes) && used + 1 < len; i++) {
		if ((mask & mps_xnu_classes[i].flag) == 0)
			continue;
		w = snprintf(buf + used, len - used, "%s%s", used ? "," : "",
		    mps_xnu_classes[i].name);
		if (w < 0)
			break;
		used += (size_t)w;
	}
	if (used == 0)
		snprintf(buf, len, "none");
}

/* ---- %b ------------------------------------------------------------------- */

const char *
mps_xnu_bits(char *buf, size_t len, uint32_t v, const char *bits)
{
	const char *name;
	size_t used;
	int w, bit, any = 0;

	w = snprintf(buf, len, (bits != NULL && *bits == 8) ? "0%o" : "0x%x",
	    v);
	if (w < 0 || (size_t)w >= len)
		return (buf);
	used = (size_t)w;
	if (bits == NULL || v == 0)
		return (buf);
	bits++;				/* the base */
	while ((bit = (unsigned char)*bits++) != 0) {
		name = bits;
		while (*bits > 32)	/* names are printable, bits 1..32 */
			bits++;
		if (bit <= 32 && (v & (1u << (bit - 1)))) {
			w = snprintf(buf + used, len - used, "%c%.*s",
			    any ? ',' : '<', (int)(bits - name), name);
			if (w < 0 || used + (size_t)w >= len)
				return (buf);
			used += (size_t)w;
			any = 1;
		}
	}
	if (any && used + 1 < len) {
		buf[used++] = '>';
		buf[used] = '\0';
	}
	return (buf);
}

/* ---- decoders --------------------------------------------------------------- */

static struct mps_table_lookup mps_xnu_ioc_state_names[] = {
	{"RESET",		0x0},
	{"READY",		0x1},
	{"OPERATIONAL",		0x2},
	{"FAULT",		0x4},
	{"COREDUMP",		0x5},
	{NULL, 0},
	{"UNKNOWN",		0x0}
};

static struct mps_table_lookup mps_xnu_tm_type_names[] = {
	{"ABORT_TASK",		MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK},
	{"ABORT_TASK_SET",	MPI2_SCSITASKMGMT_TASKTYPE_ABRT_TASK_SET},
	{"TARGET_RESET",	MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET},
	{"LOGICAL_UNIT_RESET",	MPI2_SCSITASKMGMT_TASKTYPE_LOGICAL_UNIT_RESET},
	{"CLEAR_TASK_SET",	MPI2_SCSITASKMGMT_TASKTYPE_CLEAR_TASK_SET},
	{"QUERY_TASK",		MPI2_SCSITASKMGMT_TASKTYPE_QUERY_TASK},
	{"CLEAR_ACA",		MPI2_SCSITASKMGMT_TASKTYPE_CLR_ACA},
	{"QUERY_TASK_SET",	MPI2_SCSITASKMGMT_TASKTYPE_QRY_TASK_SET},
	{"QUERY_ASYNC_EVENT",	MPI2_SCSITASKMGMT_TASKTYPE_QRY_ASYNC_EVENT},
	{NULL, 0},
	{"UNKNOWN_TM",		0x00}
};

/* SPC-4 sense keys (CAM: scsi_sense_key_text). */
static struct mps_table_lookup mps_xnu_sense_key_names[] = {
	{"NO SENSE", 0x0}, {"RECOVERED ERROR", 0x1}, {"NOT READY", 0x2},
	{"MEDIUM ERROR", 0x3}, {"HARDWARE ERROR", 0x4},
	{"ILLEGAL REQUEST", 0x5}, {"UNIT ATTENTION", 0x6},
	{"DATA PROTECT", 0x7}, {"BLANK CHECK", 0x8}, {"VENDOR SPECIFIC", 0x9},
	{"COPY ABORTED", 0xA}, {"ABORTED COMMAND", 0xB},
	{"VOLUME OVERFLOW", 0xD}, {"MISCOMPARE", 0xE}, {"COMPLETED", 0xF},
	{NULL, 0},
	{"RESERVED", 0x0}
};

/* CAM: scsi_op_desc() (what a disk sees from IOSCSIBlockCommandsDevice). */
static struct mps_table_lookup mps_xnu_cdb_names[] = {
	{"TEST UNIT READY", 0x00}, {"REQUEST SENSE", 0x03},
	{"READ(6)", 0x08}, {"WRITE(6)", 0x0A}, {"INQUIRY", 0x12},
	{"MODE SELECT(6)", 0x15}, {"MODE SENSE(6)", 0x1A},
	{"START STOP UNIT", 0x1B}, {"SEND DIAGNOSTIC", 0x1D},
	{"PREVENT ALLOW MEDIUM REMOVAL", 0x1E}, {"READ CAPACITY(10)", 0x25},
	{"READ(10)", 0x28}, {"WRITE(10)", 0x2A}, {"VERIFY(10)", 0x2F},
	{"SYNCHRONIZE CACHE(10)", 0x35}, {"WRITE SAME(10)", 0x41},
	{"UNMAP", 0x42}, {"LOG SENSE", 0x4D}, {"MODE SELECT(10)", 0x55},
	{"MODE SENSE(10)", 0x5A}, {"ATA PASS-THROUGH(16)", 0x85},
	{"READ(16)", 0x88}, {"WRITE(16)", 0x8A}, {"VERIFY(16)", 0x8F},
	{"SYNCHRONIZE CACHE(16)", 0x91}, {"WRITE SAME(16)", 0x93},
	{"SERVICE ACTION IN(16)", 0x9E}, {"REPORT LUNS", 0xA0},
	{"ATA PASS-THROUGH(12)", 0xA1}, {"READ(12)", 0xA8},
	{"WRITE(12)", 0xAA},
	{NULL, 0},
	{"CDB", 0x00}
};

const char *
mps_xnu_ioc_state_str(uint32_t doorbell)
{
	return (mps_describe_table(mps_xnu_ioc_state_names,
	    (doorbell & MPI2_IOC_STATE_MASK) >> MPI2_IOC_STATE_SHIFT));
}

const char *
mps_xnu_tm_type_str(uint8_t type)
{
	return (mps_describe_table(mps_xnu_tm_type_names, type));
}

const char *
mps_xnu_sense_key_str(uint8_t key)
{
	return (mps_describe_table(mps_xnu_sense_key_names, key & 0xF));
}

const char *
mps_xnu_cdb_str(uint8_t opcode)
{
	return (mps_describe_table(mps_xnu_cdb_names, opcode));
}

const char *
mps_xnu_asc_str(uint8_t asc, uint8_t ascq)
{
	/* CAM: asc_table (subset); any: matches every ASCQ */
	static const struct {
		uint8_t		asc, ascq, any;
		const char	*s;
	} t[] = {
		{ 0x00, 0x00, 0, "NO ADDITIONAL SENSE INFORMATION" },
		{ 0x00, 0x1D, 0, "ATA PASS THROUGH INFORMATION AVAILABLE" },
		{ 0x04, 0x00, 0, "LOGICAL UNIT NOT READY, CAUSE NOT REPORTABLE" },
		{ 0x04, 0x01, 0, "LOGICAL UNIT IS IN PROCESS OF BECOMING READY" },
		{ 0x04, 0x02, 0, "LOGICAL UNIT NOT READY, INITIALIZING COMMAND REQUIRED" },
		{ 0x04, 0x03, 0, "LOGICAL UNIT NOT READY, MANUAL INTERVENTION REQUIRED" },
		{ 0x04, 0x04, 0, "LOGICAL UNIT NOT READY, FORMAT IN PROGRESS" },
		{ 0x04, 0x00, 1, "LOGICAL UNIT NOT READY" },
		{ 0x0B, 0x01, 0, "WARNING - SPECIFIED TEMPERATURE EXCEEDED" },
		{ 0x0C, 0x00, 0, "WRITE ERROR" },
		{ 0x10, 0x01, 0, "LOGICAL BLOCK GUARD CHECK FAILED" },
		{ 0x11, 0x00, 0, "UNRECOVERED READ ERROR" },
		{ 0x14, 0x01, 0, "RECORD NOT FOUND" },
		{ 0x15, 0x01, 0, "MECHANICAL POSITIONING ERROR" },
		{ 0x1A, 0x00, 0, "PARAMETER LIST LENGTH ERROR" },
		{ 0x20, 0x00, 0, "INVALID COMMAND OPERATION CODE" },
		{ 0x21, 0x00, 0, "LOGICAL BLOCK ADDRESS OUT OF RANGE" },
		{ 0x24, 0x00, 0, "INVALID FIELD IN CDB" },
		{ 0x25, 0x00, 0, "LOGICAL UNIT NOT SUPPORTED" },
		{ 0x26, 0x00, 0, "INVALID FIELD IN PARAMETER LIST" },
		{ 0x27, 0x00, 0, "WRITE PROTECTED" },
		{ 0x28, 0x00, 0, "NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED" },
		{ 0x29, 0x00, 0, "POWER ON, RESET, OR BUS DEVICE RESET OCCURRED" },
		{ 0x29, 0x01, 0, "POWER ON OCCURRED" },
		{ 0x29, 0x02, 0, "SCSI BUS RESET OCCURRED" },
		{ 0x29, 0x03, 0, "BUS DEVICE RESET FUNCTION OCCURRED" },
		{ 0x29, 0x04, 0, "DEVICE INTERNAL RESET" },
		{ 0x2A, 0x01, 0, "MODE PARAMETERS CHANGED" },
		{ 0x2A, 0x09, 0, "CAPACITY DATA HAS CHANGED" },
		{ 0x31, 0x00, 0, "MEDIUM FORMAT CORRUPTED" },
		{ 0x32, 0x00, 0, "NO DEFECT SPARE LOCATION AVAILABLE" },
		{ 0x3A, 0x00, 0, "MEDIUM NOT PRESENT" },
		{ 0x3E, 0x02, 0, "TIMEOUT ON LOGICAL UNIT" },
		{ 0x3F, 0x0E, 0, "REPORTED LUNS DATA HAS CHANGED" },
		{ 0x44, 0x00, 0, "INTERNAL TARGET FAILURE" },
		{ 0x47, 0x00, 0, "SCSI PARITY ERROR" },
		{ 0x4B, 0x00, 0, "DATA PHASE ERROR" },
		{ 0x4E, 0x00, 0, "OVERLAPPED COMMANDS ATTEMPTED" },
		{ 0x5D, 0x00, 0, "FAILURE PREDICTION THRESHOLD EXCEEDED" },
		{ 0x5D, 0xFF, 0, "FAILURE PREDICTION THRESHOLD EXCEEDED (FALSE)" },
		{ 0x5D, 0x00, 1, "FAILURE PREDICTION THRESHOLD EXCEEDED" },
	};
	size_t i;

	for (i = 0; i < nitems(t); i++)
		if (!t[i].any && t[i].asc == asc && t[i].ascq == ascq)
			return (t[i].s);
	for (i = 0; i < nitems(t); i++)
		if (t[i].any && t[i].asc == asc)
			return (t[i].s);
	return (NULL);
}

int
mps_xnu_parse_sense(const uint8_t *s, uint32_t len, uint8_t *key,
    uint8_t *asc, uint8_t *ascq)
{
	uint8_t code;

	if (s == NULL || len < 1)
		return (0);
	code = s[0] & 0x7F;
	if ((code == 0x70 || code == 0x71) && len >= 14) {
		*key = s[2] & 0x0F;
		*asc = s[12];
		*ascq = s[13];
		return (1);
	}
	if ((code == 0x72 || code == 0x73) && len >= 4) {
		*key = s[1] & 0x0F;
		*asc = s[2];
		*ascq = s[3];
		return (1);
	}
	return (0);
}

void
mps_xnu_loginfo_str(uint32_t li, char *buf, size_t len)
{
	static const char *const orig[] = { "IOP", "PL", "IR", "?" };
	uint32_t bus, o;

	if (li == 0) {
		snprintf(buf, len, "0");
		return;
	}
	bus = li >> 28;
	o = (li >> 24) & 0xF;
	snprintf(buf, len, "0x%08x (%s %s code 0x%02x sub 0x%04x)", li,
	    bus == 3 ? "SAS" : "bus?", orig[o < 3 ? o : 3], (li >> 16) & 0xFF,
	    li & 0xFFFF);
}

void
mps_xnu_lun_str(const uint8_t l[8], char *buf, size_t len)
{
	int i, rest0 = 1;
	uint8_t method;

	for (i = 2; i < 8; i++)
		if (l[i] != 0)
			rest0 = 0;
	method = l[0] >> 6;
	if (rest0 && (method == 0 || method == 1))
		snprintf(buf, len, "lun %u",
		    (unsigned)(((l[0] & 0x3F) << 8) | l[1]));
	else
		snprintf(buf, len, "lun %02x%02x%02x%02x%02x%02x%02x%02x",
		    l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7]);
}

int
mps_xnu_cdb_lba(const uint8_t *c, uint8_t n, uint64_t *lba, uint32_t *blocks)
{
	uint64_t v;
	int i;

	if (n == 0)
		return (0);
	switch (c[0]) {
	case 0x08: case 0x0A:
		if (n < 6)
			return (0);
		*lba = ((uint64_t)(c[1] & 0x1F) << 16) | ((uint64_t)c[2] << 8) |
		    c[3];
		*blocks = c[4] ? c[4] : 256;
		return (1);
	case 0x28: case 0x2A: case 0x2F:
		if (n < 10)
			return (0);
		*lba = scsi_4btoul(&c[2]);
		*blocks = scsi_2btoul(&c[7]);
		return (1);
	case 0xA8: case 0xAA:
		if (n < 12)
			return (0);
		*lba = scsi_4btoul(&c[2]);
		*blocks = scsi_4btoul(&c[6]);
		return (1);
	case 0x88: case 0x8A: case 0x8F:
		if (n < 16)
			return (0);
		for (v = 0, i = 2; i < 10; i++)
			v = (v << 8) | c[i];
		*lba = v;
		*blocks = scsi_4btoul(&c[10]);
		return (1);
	default:
		return (0);
	}
}

/* ---- rate limiter ------------------------------------------------------- */

int
mps_xnu_ratelimit_allow(struct mps_xnu_ratelimit *rl, uint64_t now_ms,
    uint32_t burst, uint32_t window_ms, uint32_t *suppressed)
{
	*suppressed = 0;
	if (rl->window_start_ms == 0 ||
	    now_ms - rl->window_start_ms >= window_ms) {
		*suppressed = rl->suppressed;
		rl->suppressed = 0;
		rl->window_start_ms = now_ms ? now_ms : 1;
		rl->in_window = 1;
		return (1);
	}
	if (rl->in_window < burst) {
		rl->in_window++;
		return (1);
	}
	rl->suppressed++;
	return (0);
}

int
mps_xnu_ratelimit_flush(struct mps_xnu_ratelimit *rl, uint64_t now_ms,
    uint32_t window_ms, uint32_t *suppressed)
{
	*suppressed = 0;
	if (rl->suppressed == 0 || now_ms - rl->window_start_ms < window_ms)
		return (0);
	*suppressed = rl->suppressed;
	rl->suppressed = 0;
	rl->window_start_ms = 0;	/* the next line opens a new window */
	rl->in_window = 0;
	return (1);
}

/* ---- target ids ----------------------------------------------------------- */

/*
 * MPSSAS_TARGET_XNU_REMOVE_WAIT (mps_sas.h) must be a bit of its own in the
 * uint8_t flags: FreeBSD uses bits 0-6, bit 6 is MPS_TARGET_IS_SATA_SSD (D1).
 */
_Static_assert(MPSSAS_TARGET_XNU_REMOVE_WAIT <
    (1 << (8 * sizeof(((struct mpssas_target *)0)->flags))),
    "REMOVE_WAIT does not fit mpssas_target.flags");
_Static_assert((MPSSAS_TARGET_XNU_REMOVE_WAIT & (MPSSAS_TARGET_INABORT |
    MPSSAS_TARGET_INRESET | MPSSAS_TARGET_INDIAGRESET |
    MPSSAS_TARGET_INREMOVAL | MPS_TARGET_FLAGS_RAID_COMPONENT |
    MPS_TARGET_FLAGS_VOLUME | MPS_TARGET_IS_SATA_SSD)) == 0,
    "REMOVE_WAIT overlaps another target flag");

/*
 * Target ids (FreeBSD: mps_mapping.c, keyed on the SAS address, which
 * needs config pages).  Here: the id that last had this device identity
 * (`ident': SAS address, IDENTIFY-derived for SATA) comes back to it, else
 * the first id never used, else the first free one.  Handles do not
 * identify disks: on this IT firmware a direct-attached handle follows the
 * PHY, so the same bay always gives the same handle.  Whenever an id is
 * given to a different (or unknown, ident 0) device its generation
 * changes; the OS compares generations and never keeps a nub across a
 * change.
 */
u_int
mps_xnu_get_tid(struct mps_softc *sc, uint16_t handle, uint64_t ident)
{
	struct mpssas_softc *sassc = sc->sassc;
	u_int i, n, never = MPS_MAP_BAD_ID, freeid = MPS_MAP_BAD_ID;

	n = MIN(sassc->maxtargets, MPS_XNU_MAX_TARGETS);
	if (ident != 0) {
		for (i = 0; i < n; i++) {
			if (sc->xnu.tid_ident[i] == ident &&
			    sassc->targets[i].handle == 0) {
				sc->xnu.tid_handle[i] = handle;
				return (i);
			}
		}
	} else {
		/* unknown identity: the id of this handle, as a new disk */
		for (i = 0; i < n; i++) {
			if (sc->xnu.tid_handle[i] == handle &&
			    sassc->targets[i].handle == 0) {
				freeid = i;
				break;
			}
		}
	}
	for (i = 0; i < n && freeid == MPS_MAP_BAD_ID; i++) {
		if (sassc->targets[i].handle != 0)
			continue;
		if (sc->xnu.tid_handle[i] == 0 && sc->xnu.tid_ident[i] == 0) {
			never = i;
			break;
		}
	}
	for (i = 0; i < n && freeid == MPS_MAP_BAD_ID &&
	    never == MPS_MAP_BAD_ID; i++) {
		if (sassc->targets[i].handle == 0)
			freeid = i;
	}
	i = (freeid != MPS_MAP_BAD_ID) ? freeid : never;
	if (i == MPS_MAP_BAD_ID)
		return (i);
	if (sc->xnu.tid_handle[i] != 0 || sc->xnu.tid_ident[i] != 0) {
		sc->xnu.stats.identity_changes++;
		mps_dprint(sc, MPS_MAPPING|MPS_INFO, "target id %u: a "
		    "different device (handle 0x%04x, id 0x%016jx, was 0x%016jx)"
		    "\n", i, handle, (uintmax_t)ident,
		    (uintmax_t)sc->xnu.tid_ident[i]);
	}
	sc->xnu.tid_gen[i]++;
	sc->xnu.tid_handle[i] = handle;
	sc->xnu.tid_ident[i] = ident;
	return (i);
}

uint32_t
mps_xnu_target_gen(struct mps_softc *sc, u_int tid)
{
	return (tid < MPS_XNU_MAX_TARGETS ? sc->xnu.tid_gen[tid] : 0);
}

int
mps_xnu_target_present(struct mps_softc *sc, u_int tid, uint16_t *handle)
{
	struct mpssas_target *targ;

	if (sc->sassc == NULL || tid >= sc->sassc->maxtargets)
		return (0);
	targ = &sc->sassc->targets[tid];
	if (targ->handle == 0 || (targ->flags & MPSSAS_TARGET_INREMOVAL) ||
	    (sc->mps_flags & MPS_FLAGS_XNU_GONE))
		return (0);
	if (handle != NULL)
		*handle = targ->handle;
	return (1);
}

/* ---- surprise removal, controller disabled ----------------------------- */

void
mps_xnu_gone(struct mps_softc *sc, uint32_t offset)
{
	if (sc->mps_flags & MPS_FLAGS_XNU_GONE)
		return;
	sc->mps_flags |= MPS_FLAGS_XNU_GONE;
	mps_dprint(sc, MPS_FAULT, "register 0x%02x reads 0xffffffff: "
	    "controller gone (surprise removal)\n", offset);
	/* the reinit task fails everything (mps_reinit -> mps_xnu_disable) */
	mps_xnu_request_reinit(sc, "surprise removal");
}

void
mps_xnu_disable(struct mps_softc *sc, int error)
{
	if (sc->xnu.disabled)
		return;
	sc->xnu.disabled = 1;
	if (sc->mps_flags & MPS_FLAGS_XNU_GONE)
		mps_dprint(sc, MPS_FAULT, "surprise removal: controller "
		    "disabled\n");
	else
		mps_dprint(sc, MPS_FAULT, "reinit failed (error %d) after %d "
		    "attempts: controller disabled\n", error,
		    MPS_XNU_REINIT_TRIES);
	sc->mps_flags |= MPS_FLAGS_XNU_GONE;
	mps_os_controller_dead(sc->mps_dev);
	if (sc->sassc != NULL && sc->sassc->sim != NULL) {
		/*
		 * Posted commands, TMs and IO held for the reinit are
		 * completed (CAM_DEV_NOT_THERE); the OS drops the targets.
		 */
		mpssas_xnu_fail_all(sc);
		mpssas_rescan_target(sc, NULL);
	}
}

/* ---- event acks ------------------------------------------------------------- */

static void
mps_xnu_event_ack_complete(struct mps_softc *sc, struct mps_command *cm)
{
	MPI2_EVENT_ACK_REPLY *reply;

	reply = (MPI2_EVENT_ACK_REPLY *)cm->cm_reply;
	if (reply != NULL && (le16toh(reply->IOCStatus) &
	    MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS)
		mps_dprint(sc, MPS_ERROR, "event ack failed, IOCStatus 0x%x\n",
		    le16toh(reply->IOCStatus));
	else
		mps_dprint(sc, MPS_XINFO, "event ack done\n");
	mps_free_command(sc, cm);
}

void
mps_xnu_event_ack(struct mps_softc *sc, void *event_reply)
{
	MPI2_EVENT_NOTIFICATION_REPLY *event = event_reply;
	MPI2_EVENT_ACK_REQUEST *req;
	struct mps_command *cm;

	if (event->AckRequired != MPI2_EVENT_NOTIFICATION_ACK_REQUIRED)
		return;
	if (sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE))
		return;
	if ((cm = mps_alloc_command(sc)) == NULL) {
		sc->xnu.stats.event_ack_dropped++;
		mps_dprint(sc, MPS_ERROR, "no command for the ack of event "
		    "%s (context 0x%x)\n", mps_describe_table(mps_event_names,
		    le16toh(event->Event)), le32toh(event->EventContext));
		return;
	}
	req = (MPI2_EVENT_ACK_REQUEST *)cm->cm_req;
	bzero(req, sizeof(*req));
	req->Function = MPI2_FUNCTION_EVENT_ACK;
	req->Event = event->Event;
	req->EventContext = event->EventContext;
	cm->cm_desc.Default.RequestFlags = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE;
	cm->cm_complete = mps_xnu_event_ack_complete;
	cm->cm_data = NULL;
	cm->cm_sge = NULL;
	mps_dprint(sc, MPS_XINFO, "ack event %s (context 0x%x)\n",
	    mps_describe_table(mps_event_names, le16toh(event->Event)),
	    le32toh(event->EventContext));
	mps_map_command(sc, cm);
}

/* ---- statistics ------------------------------------------------------------ */

void
mps_xnu_stats_update(struct mps_softc *sc)
{
	sc->xnu.stats.chain_alloc_fail = (uint32_t)sc->chain_alloc_fail;
	sc->xnu.stats.chain_free_low = (uint32_t)sc->chain_free_lowwater;
	if (sc->sassc != NULL && sc->sassc->sim != NULL)
		sc->xnu.stats.held = sc->sassc->sim->held_total;
}

/* ---- reinit requests, fault check ------------------------------------------ */

void
mps_xnu_request_reinit(struct mps_softc *sc, const char *why)
{
	if ((sc->mps_flags & MPS_FLAGS_SHUTDOWN) || sc->mps_tq == NULL)
		return;
	/*
	 * FreeBSD's mps_reinit returns "Reset already in progress": the reset
	 * that runs covers this request.  Recording it would leave
	 * reinit_pending set after that reset (new IO requeued for good) or
	 * run a second diag reset.  Asleep, MPS_FLAGS_DIAGRESET stays set
	 * until the resume, which reinitializes anyway.
	 */
	if ((sc->mps_flags & MPS_FLAGS_DIAGRESET) && !sc->xnu.suspending) {
		sc->xnu.stats.reinit_coalesced++;
		mps_dprint(sc, MPS_RECOVERY, "controller reinit requested: "
		    "%s; the reset in progress covers it\n", why);
		return;
	}
	if (!sc->xnu.reinit_pending) {
		sc->xnu.reinit_pending = 1;
		mps_dprint(sc, MPS_RECOVERY, "controller reinit requested: "
		    "%s\n", why);
	}
	taskqueue_enqueue(sc->mps_tq, &sc->mps_reinit_task);
}

void
mps_xnu_check_fault(struct mps_softc *sc)
{
	uint32_t db;

	if (sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE |
	    MPS_FLAGS_SHUTDOWN) || sc->xnu.reinit_pending)
		return;
	db = mps_regread(sc, MPI2_DOORBELL_OFFSET);
	if ((db & MPI2_IOC_STATE_MASK) != MPI2_IOC_STATE_FAULT ||
	    (sc->mps_flags & MPS_FLAGS_XNU_GONE))
		return;
	mps_dprint(sc, MPS_FAULT, "IOC Fault 0x%08x, Resetting (found by "
	    "an interrupt)\n", db);
	sc->xnu.stats.faults++;
	mps_xnu_request_reinit(sc, "IOC fault");
}

static void	mps_xnu_queue_due(struct mps_softc *sc);

void
mps_xnu_periodic(struct mps_softc *sc)
{
	uint32_t sup;
	int i;

	for (i = 0; i < MPS_XNU_RL_SLOTS; i++) {
		if (mps_xnu_ratelimit_flush(&sc->xnu.ioerr_rl[i],
		    mps_os_uptime_us() / 1000, MPS_XNU_RL_WINDOW_MS, &sup))
			mps_dprint(sc, MPS_ERROR, "target %u (handle 0x%04x): "
			    "%u similar IO error lines suppressed\n",
			    sc->xnu.ioerr_tid[i], sc->xnu.ioerr_handle[i], sup);
	}
	mps_xnu_queue_due(sc);
	mps_xnu_stats_update(sc);
}

/* ---- SCSI IO: trace lines, statistics, failed-IO line ---------------------- */

static int
mps_xnu_chains(struct mps_command *cm)
{
	struct mps_chain *chain;
	int n = 0;

	TAILQ_FOREACH(chain, &cm->cm_chain_list, chain_link)
		n++;
	return (n);
}

static const uint8_t *
mps_xnu_cdb(union ccb *ccb)
{
	return ((ccb->ccb_h.flags & CAM_CDB_POINTER) ?
	    ccb->csio.cdb_io.cdb_ptr : ccb->csio.cdb_io.cdb_bytes);
}

void
mps_xnu_io_start(struct mps_softc *sc, struct mps_command *cm)
{
	union ccb *ccb = cm->cm_ccb;
	const uint8_t *cdb;
	uint64_t lba = 0;
	uint32_t blocks = 0;

	if ((sc->mps_debug & MPS_TRACE) == 0)
		return;
	cdb = mps_xnu_cdb(ccb);
	mps_xnu_cdb_lba(cdb, ccb->csio.cdb_len, &lba, &blocks);
	mps_dprint(sc, MPS_TRACE, "IO start SMID %u target %u (handle "
	    "0x%04x) lun %ju %s lba %ju blocks %u len %u segs %u chains %d\n",
	    cm->cm_desc.Default.SMID, ccb->ccb_h.target_id,
	    cm->cm_targ->handle, (uintmax_t)ccb->ccb_h.target_lun,
	    mps_xnu_cdb_str(cdb[0]), (uintmax_t)lba, blocks,
	    ccb->csio.dxfer_len, (cm->cm_length != 0) ?
	    ccb->csio.sglist_cnt : 0, mps_xnu_chains(cm));
}

void
mps_xnu_io_done(struct mps_softc *sc, struct mps_command *cm, union ccb *ccb,
    void *scsi_io_reply)
{
	MPI2_SCSI_IO_REPLY *rep = scsi_io_reply;
	struct mps_xnu_stats *st = &sc->xnu.stats;
	struct ccb_scsiio *csio = &ccb->csio;
	uint32_t status = ccb->ccb_h.status & CAM_STATUS_MASK;
	uint16_t ioc = 0;
	uint64_t now = mps_os_uptime_us(), lat;
	uint32_t xfer = 0, sup, slot;
	uint8_t key = 0, asc = 0, ascq = 0;
	uint64_t lba = 0;
	uint32_t blocks = 0;
	const uint8_t *cdb = mps_xnu_cdb(ccb);
	const char *ascs;
	char li[64], xf[48];
	struct mpssas_target *targ = cm->cm_targ;
	int sense, recovered;
	uint32_t cls;

	if (status == CAM_REQUEUE_REQ)		/* not done: comes back */
		return;
	if (rep != NULL)
		ioc = le16toh(rep->IOCStatus) & MPI2_IOCSTATUS_MASK;
	/* the IOC's sense buffer (FreeBSD copies it only for some IOCStatus) */
	sense = rep != NULL &&
	    (rep->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_VALID) &&
	    mps_xnu_parse_sense((const uint8_t *)cm->cm_sense,
	    min(le32toh(rep->SenseCount), MPS_SENSE_LEN), &key, &asc, &ascq);
	/*
	 * CHECK CONDITION with RECOVERED ERROR: the command completed.  SAT
	 * returns the ATA registers of an ATA PASS-THROUGH(12/16) with CK_COND
	 * this way (ASC/ASCQ 0x00/0x1D, ATA PASS THROUGH INFORMATION
	 * AVAILABLE): macOS SMART polling does it every few minutes.  Not an
	 * error: counted apart, logged at xinfo.
	 */
	recovered = status == CAM_SCSI_STATUS_ERROR && sense &&
	    csio->scsi_status == MPI2_SCSI_STATUS_CHECK_CONDITION &&
	    key == SSD_KEY_RECOVERED_ERROR;
	if (status == CAM_REQ_CMP || status == CAM_SCSI_STATUS_ERROR)
		xfer = cm->cm_length - csio->resid;
	lat = now - ccb->ccb_h.xnu_start_us;
	st->ios++;
	st->lat_sum_us += lat;
	if (lat > st->lat_max_us)
		st->lat_max_us = (uint32_t)lat;
	if (status == CAM_REQ_CMP) {
		if (cm->cm_flags & MPS_CM_FLAGS_DATAIN)
			st->read_bytes += xfer;
		else if (cm->cm_flags & MPS_CM_FLAGS_DATAOUT)
			st->write_bytes += xfer;
	}
	switch (status) {
	case CAM_REQ_CMP:
		break;
	case CAM_SCSI_STATUS_ERROR:
		if (recovered)
			st->recovered_sense++;
		else if (csio->scsi_status == MPI2_SCSI_STATUS_CHECK_CONDITION)
			st->err_check_condition++;
		else
			st->err_scsi_status++;
		break;
	case CAM_CMD_TIMEOUT:
	case CAM_REQ_ABORTED:
	case CAM_SCSI_BUS_RESET:
		st->err_terminated++;
		break;
	case CAM_DEV_NOT_THERE:
		st->err_device_gone++;
		break;
	default:
		if (ioc == MPI2_IOCSTATUS_SCSI_IOC_TERMINATED ||
		    ioc == MPI2_IOCSTATUS_SCSI_EXT_TERMINATED ||
		    ioc == MPI2_IOCSTATUS_SCSI_TASK_TERMINATED)
			st->err_terminated++;
		else
			st->err_transport++;
		break;
	}

	if (sc->mps_debug & MPS_TRACE)
		mps_dprint(sc, MPS_TRACE, "IO done SMID %u cam status 0x%x "
		    "scsi %s xfer %u/%u latency %ju us\n",
		    cm->cm_desc.Default.SMID, status,
		    mps_describe_table(mps_scsi_status_string,
		    csio->scsi_status), xfer, cm->cm_length, (uintmax_t)lat);

	/*
	 * The failed-IO line: a reply frame with an error (not a termination
	 * by our own task management).  Error lines are rate limited per
	 * target; xinfo lines (debug level, opt-in) are not.
	 *
	 * XNU: class as FreeBSD.  A command the target completed with a SCSI
	 * status (CAM_SCSI_STATUS_ERROR: IOCStatus success, data underrun or
	 * recovered error, status not GOOD: check condition, busy, task set
	 * full, ...) is printed by mps only at MPS_XINFO
	 * (mpssas_scsiio_complete: mpssas_log_command(cm, MPS_XINFO, "ioc %x
	 * scsi %x state %x xfer %u"), mps_sc_failed_io_info); the periph
	 * driver decides what the status means.  So xinfo here: os_log DEBUG,
	 * not persisted.  Spun-down SATA drives answer the family's TEST UNIT
	 * READY polls (~3 s) with NOT READY 0x04/0x02, ~140 ERROR lines a
	 * minute before.  Exception: CHECK CONDITION with sense key MEDIUM
	 * ERROR or HARDWARE ERROR stays error: FreeBSD's CAM prints that sense
	 * on the console (scsi_all.c sense_key_table: SS_RDEF includes
	 * SSQ_PRINT_SENSE; cam_periph_error -> cam_error_print) while 0x04/0x02
	 * is SS_START, silent; IOSCSIArchitectureModelFamily logs no sense, so
	 * this line is the only trace of a failing disk.  Not ABORTED COMMAND
	 * (also SS_RDEF): SAT maps an ATA ABRT (e.g. an unsupported command
	 * passed through) to it.  Every other CAM status (IOC and
	 * transport failures, autosense failed, device gone) stays error.
	 * Counters are unchanged: every check condition is "Err check
	 * condition".
	 */
	if (recovered)
		cls = MPS_XINFO;
	else if (status == CAM_SCSI_STATUS_ERROR &&
	    !(sense && csio->scsi_status == MPI2_SCSI_STATUS_CHECK_CONDITION &&
	    (key == SSD_KEY_MEDIUM_ERROR || key == SSD_KEY_HARDWARE_ERROR)))
		cls = MPS_XINFO;
	else
		cls = MPS_ERROR;
	if (rep == NULL || status == CAM_REQ_CMP ||
	    ioc == MPI2_IOCSTATUS_SCSI_TASK_TERMINATED ||
	    (sc->mps_debug & cls) == 0)
		return;
	sup = 0;
	if (cls == MPS_ERROR) {
		slot = targ->handle % MPS_XNU_RL_SLOTS;
		sc->xnu.ioerr_handle[slot] = targ->handle;
		sc->xnu.ioerr_tid[slot] = targ->tid;
		if (!mps_xnu_ratelimit_allow(&sc->xnu.ioerr_rl[slot],
		    now / 1000, MPS_XNU_RL_BURST, MPS_XNU_RL_WINDOW_MS, &sup))
			return;
	}
	if (sup != 0)
		mps_dprint(sc, MPS_ERROR, "target %u (handle 0x%04x): %u "
		    "similar IO error lines suppressed\n", targ->tid,
		    targ->handle, sup);
	mps_xnu_cdb_lba(cdb, csio->cdb_len, &lba, &blocks);
	li[0] = '\0';
	if (le16toh(rep->IOCStatus) & MPI2_IOCSTATUS_FLAG_LOG_INFO_AVAILABLE) {
		char d[48];

		mps_xnu_loginfo_str(le32toh(rep->IOCLogInfo), d, sizeof(d));
		snprintf(li, sizeof(li), " loginfo %s", d);
	}
	/*
	 * XNU: the transfer as "len N IOC TransferCount M": N is the request's
	 * DataLength (dxfer_len), M the reply frame's field, which like
	 * FreeBSD we use only for DATA_UNDERRUN (resid).  A no-data CDB is
	 * sent with DataLength 0 and NODATATRANSFER; its reply's
	 * TransferCount is not meaningful (SAS2308 on SATA TEST UNIT READY:
	 * 0, 512, ... 65536, mostly 65536), so it is not printed: "no data".
	 */
	if (cm->cm_length == 0)
		snprintf(xf, sizeof(xf), "no data");
	else
		snprintf(xf, sizeof(xf), "len %u IOC TransferCount %u",
		    cm->cm_length, le32toh(rep->TransferCount));
	if (sense) {
		ascs = mps_xnu_asc_str(asc, ascq);
		mps_dprint(sc, cls, "target %u (handle 0x%04x) lun %ju "
		    "SMID %u %s lba %ju blocks %u %s: IOCStatus %s "
		    "(0x%04x)%s SCSI status %s (0x%02x) state 0x%02x sense %s "
		    "(0x%x) ASC/ASCQ 0x%02x/0x%02x%s%s %s\n",
		    targ->tid, targ->handle, (uintmax_t)ccb->ccb_h.target_lun,
		    cm->cm_desc.Default.SMID, mps_xnu_cdb_str(cdb[0]),
		    (uintmax_t)lba, blocks, recovered ? "recovered" : "failed",
		    mps_describe_table(mps_iocstatus_string, ioc), ioc, li,
		    mps_describe_table(mps_scsi_status_string, rep->SCSIStatus),
		    rep->SCSIStatus, rep->SCSIState, mps_xnu_sense_key_str(key),
		    key, asc, ascq, ascs ? " " : "", ascs ? ascs : "", xf);
	} else {
		mps_dprint(sc, cls, "target %u (handle 0x%04x) lun %ju "
		    "SMID %u %s lba %ju blocks %u failed: IOCStatus %s "
		    "(0x%04x)%s SCSI status %s (0x%02x) state 0x%02x "
		    "%s\n", targ->tid, targ->handle,
		    (uintmax_t)ccb->ccb_h.target_lun,
		    cm->cm_desc.Default.SMID, mps_xnu_cdb_str(cdb[0]),
		    (uintmax_t)lba, blocks,
		    mps_describe_table(mps_iocstatus_string, ioc), ioc, li,
		    mps_describe_table(mps_scsi_status_string, rep->SCSIStatus),
		    rep->SCSIStatus, rep->SCSIState, xf);
	}
}

void
mps_xnu_tm_done(struct mps_softc *sc, struct mps_command *tm)
{
	MPI2_SCSI_TASK_MANAGE_REQUEST *req;
	MPI2_SCSI_TASK_MANAGE_REPLY *reply;
	uint16_t ioc;

	req = (MPI2_SCSI_TASK_MANAGE_REQUEST *)tm->cm_req;
	reply = (MPI2_SCSI_TASK_MANAGE_REPLY *)tm->cm_reply;
	ioc = le16toh(reply->IOCStatus) & MPI2_IOCSTATUS_MASK;
	if (ioc != MPI2_IOCSTATUS_SUCCESS)
		sc->xnu.stats.tm_failed++;
	mps_dprint(sc, MPS_RECOVERY, "%s SMID %u to target %u (handle "
	    "0x%04x) completed in %ju ms: IOCStatus %s (0x%04x) response %s "
	    "(0x%02x) terminated %u\n", mps_xnu_tm_type_str(req->TaskType),
	    tm->cm_desc.Default.SMID, tm->cm_targ != NULL ?
	    tm->cm_targ->tid : 0xffff, le16toh(req->DevHandle),
	    (uintmax_t)((mps_os_uptime_us() - tm->cm_xnu_start_us) / 1000),
	    mps_describe_table(mps_iocstatus_string, ioc), ioc,
	    mps_describe_table(mps_scsi_taskmgmt_string, reply->ResponseCode),
	    reply->ResponseCode, le32toh(reply->TerminationCount));
}

void
mps_xnu_reinit_done(struct mps_softc *sc)
{
	uint64_t ms;

	ms = (mps_os_uptime_us() - sc->xnu.reinit_start_us) / 1000;
	if (ms > sc->xnu.stats.reinit_max_ms)
		sc->xnu.stats.reinit_max_ms = (uint32_t)ms;
	mps_dprint(sc, MPS_INIT|MPS_INFO, "reinit done in %ju ms\n",
	    (uintmax_t)ms);
}

/* ---- system sleep ------------------------------------------------------------ */

uint32_t
mps_xnu_suspended_access(struct mps_softc *sc, uint32_t offset)
{
	sc->xnu.stats.suspended_access++;
	if (!sc->xnu.suspended_logged) {
		sc->xnu.suspended_logged = 1;
		mps_dprint(sc, MPS_FAULT, "register 0x%02x accessed while "
		    "suspended: refused\n", offset);
	}
	return (0);
}

/* ---- config pages (mps_config.c) -------------------------------------------- */

void *
mps_xnu_config_page(struct mps_softc *sc, struct mps_command *cm)
{
	if (sc->xnu.cfg_busy || cm->cm_length == 0 ||
	    cm->cm_length > MPS_XNU_CFG_PAGE_BYTES)
		return (NULL);
	if (sc->xnu.cfg_page == NULL) {
		if (bus_dma_tag_create(sc->mps_parent_dmat, 4, 0,
		    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
		    MPS_XNU_CFG_PAGE_BYTES, 1, MPS_XNU_CFG_PAGE_BYTES, 0, NULL,
		    NULL, &sc->xnu.cfg_dmat) != 0)
			return (NULL);
		if (bus_dmamem_alloc(sc->xnu.cfg_dmat, &sc->xnu.cfg_page,
		    BUS_DMA_NOWAIT, &sc->xnu.cfg_map) != 0) {
			bus_dma_tag_destroy(sc->xnu.cfg_dmat);
			sc->xnu.cfg_dmat = NULL;
			sc->xnu.cfg_page = NULL;
			return (NULL);
		}
	}
	bzero(sc->xnu.cfg_page, cm->cm_length);
	bus_dmamap_xnu_lend(cm->cm_dmamap, sc->xnu.cfg_map);
	sc->xnu.cfg_busy = 1;
	return (sc->xnu.cfg_page);
}

void
mps_xnu_config_page_done(struct mps_softc *sc, void *page)
{
	if (page != NULL && page == sc->xnu.cfg_page)
		sc->xnu.cfg_busy = 0;
}

/*
 * Nothing new may be started on the event queue: reset, gone, suspended,
 * shutting down.
 */
static int
mps_xnu_quiet(struct mps_softc *sc)
{
	return ((sc->mps_flags & (MPS_FLAGS_DIAGRESET | MPS_FLAGS_XNU_GONE |
	    MPS_FLAGS_XNU_SUSPENDED | MPS_FLAGS_SHUTDOWN)) != 0 ||
	    sc->xnu.suspending || sc->xnu.reinit_pending || sc->xnu.disabled);
}

/*
 * A page read that failed on our side (no free command: EBUSY, the page
 * buffer busy: ENOMEM) or that the IOC refused for now (BUSY, insufficient
 * resources).  A wait error (timeout: mps_reinit ran; reset in progress) is
 * not: the rediscovery after the reset adds the device; a firmware error
 * (invalid page: the handle is gone) is final, as on FreeBSD.
 */
static int
mps_xnu_config_retryable(int error, const Mpi2ConfigReply_t *reply)
{
	uint16_t ioc_status;

	if (error == EBUSY || error == ENOMEM)
		return (1);
	ioc_status = le16toh(reply->IOCStatus) & MPI2_IOCSTATUS_MASK;
	return (ioc_status == MPI2_IOCSTATUS_BUSY ||
	    ioc_status == MPI2_IOCSTATUS_INSUFFICIENT_RESOURCES);
}

static const char *
mps_xnu_errno_str(int error)
{
	switch (error) {
	case EBUSY:	return ("no free command");
	case ENOMEM:	return ("page buffer busy");
	case ENXIO:	return ("no reply or IOC error");
	default:	return ("error");
	}
}

int
mps_xnu_get_sas_device_pg0(struct mps_softc *sc, void *mpi_reply,
    void *config_page, uint32_t form, uint16_t handle)
{
	int error, try;

	for (try = 1; ; try++) {
		bzero(mpi_reply, sizeof(Mpi2ConfigReply_t));
		sc->xnu.stats.config_reads++;
		error = mps_config_get_sas_device_pg0(sc, mpi_reply,
		    config_page, form, handle);
		if (error == 0 || try >= MPS_XNU_CFG_TRIES ||
		    !mps_xnu_config_retryable(error, mpi_reply) ||
		    mps_xnu_quiet(sc))
			return (error);
		sc->xnu.stats.config_retries++;
		mps_dprint(sc, MPS_MAPPING | MPS_INFO, "handle 0x%04x: SAS "
		    "device page0 read failed (error %d: %s, iocstatus 0x%x), "
		    "retry %d/%d in %d ms\n", handle, error,
		    mps_xnu_errno_str(error),
		    le16toh(((Mpi2ConfigReply_t *)mpi_reply)->IOCStatus),
		    try + 1, MPS_XNU_CFG_TRIES, 10 << (try - 1));
		if (mtx_owned(&sc->mps_mtx))
			(void)msleep(&sc->xnu.cfg_retry, &sc->mps_mtx, 0,
			    "mpscfg", (hz / 100) << (try - 1));
		else
			pause("mpscfg", (hz / 100) << (try - 1));
	}
}

static struct mps_xnu_readd *
mps_xnu_readd_find(struct mps_softc *sc, uint16_t handle)
{
	int i;

	for (i = 0; i < MPS_XNU_READD_SLOTS; i++)
		if (sc->xnu.readd[i].handle == handle)
			return (&sc->xnu.readd[i]);
	return (NULL);
}

void
mps_xnu_readd_schedule(struct mps_softc *sc, uint16_t handle,
    uint8_t linkrate, int error, const void *mpi_reply)
{
	struct mps_xnu_readd *r;
	uint16_t ioc_status;
	uint32_t ms;

	ioc_status = le16toh(((const Mpi2ConfigReply_t *)mpi_reply)->IOCStatus);
	r = mps_xnu_readd_find(sc, handle);
	if (!mps_xnu_config_retryable(error, mpi_reply) ||
	    (r != NULL && r->tries >= MPS_XNU_READD_TRIES)) {
		if (r != NULL)
			bzero(r, sizeof(*r));
		sc->xnu.stats.readd_given_up++;
		mps_dprint(sc, MPS_FAULT, "handle 0x%04x: SAS device page0 "
		    "read failed (error %d: %s, iocstatus 0x%x): device not "
		    "added%s\n", handle, error, mps_xnu_errno_str(error),
		    ioc_status, mps_xnu_config_retryable(error, mpi_reply) ?
		    ", retries exhausted" : "");
		return;
	}
	if (r == NULL && (r = mps_xnu_readd_find(sc, 0)) == NULL) {
		sc->xnu.stats.readd_given_up++;
		mps_dprint(sc, MPS_FAULT, "handle 0x%04x: SAS device page0 "
		    "read failed (error %d: %s): no re-add slot, device not "
		    "added\n", handle, error, mps_xnu_errno_str(error));
		return;
	}
	r->handle = handle;
	r->linkrate = linkrate;
	r->tries++;
	r->queued = 0;
	ms = 1000 * min(r->tries, 5);
	r->due_us = mps_os_uptime_us() + (uint64_t)ms * 1000;
	sc->xnu.stats.readd_scheduled++;
	mps_dprint(sc, MPS_FAULT, "handle 0x%04x: SAS device page0 read failed "
	    "(error %d: %s, iocstatus 0x%x): add retried in %u ms (%u/%u)\n",
	    handle, error, mps_xnu_errno_str(error), ioc_status, ms, r->tries,
	    MPS_XNU_READD_TRIES);
}

void
mps_xnu_readd_cancel(struct mps_softc *sc, uint16_t handle)
{
	struct mps_xnu_readd *r;

	/* a READD event still queued finds no entry and does nothing */
	if (handle == 0)
		bzero(sc->xnu.readd, sizeof(sc->xnu.readd));
	else if ((r = mps_xnu_readd_find(sc, handle)) != NULL)
		bzero(r, sizeof(*r));
}

int
mps_xnu_readd_begin(struct mps_softc *sc, uint16_t handle, uint8_t *linkrate)
{
	struct mps_xnu_readd *r;

	r = mps_xnu_readd_find(sc, handle);
	if (r == NULL || !r->queued)
		return (0);			/* cancelled */
	if (mpssas_find_target_by_handle(sc->sassc, 0, handle) != NULL ||
	    mps_xnu_quiet(sc)) {
		bzero(r, sizeof(*r));		/* added meanwhile / reset */
		return (0);
	}
	mps_dprint(sc, MPS_MAPPING | MPS_INFO, "handle 0x%04x: adding the "
	    "device again (%u/%u)\n", handle, r->tries, MPS_XNU_READD_TRIES);
	r->queued = 0;
	r->due_us = 0;
	*linkrate = r->linkrate;
	return (1);
}

void
mps_xnu_readd_end(struct mps_softc *sc, uint16_t handle)
{
	struct mps_xnu_readd *r;

	/* added, or failed for a reason that is not retried: forget it */
	r = mps_xnu_readd_find(sc, handle);
	if (r != NULL && r->due_us == 0)
		bzero(r, sizeof(*r));
}

/* 1 s, mps_mtx held: queue the re-adds and the temperature read that are due */
static void
mps_xnu_queue_due(struct mps_softc *sc)
{
	struct mps_xnu_readd *r;
	uint64_t now = mps_os_uptime_us();
	int i;

	if (mps_xnu_quiet(sc) || sc->sassc == NULL)
		return;
	for (i = 0; i < MPS_XNU_READD_SLOTS; i++) {
		r = &sc->xnu.readd[i];
		if (r->handle == 0 || r->queued ||
		    now < r->due_us)
			continue;
		if (mpssas_xnu_queue_work(sc, MPS_XNU_EVENT_READD, &r->handle,
		    sizeof(r->handle)) == 0)
			r->queued = 1;
	}
	/*
	 * The temperature: every MPS_XNU_TEMP_PERIOD_S, not before port enable
	 * completed nor while firmware events wait (discovery comes first), never
	 * two reads queued.  It runs on the event queue, so it cannot overlap a
	 * page 0 read.  (Not MPSSAS_IN_DISCOVERY: only a discovery event clears
	 * it.)
	 */
	if (sc->xnu.temp_absent || sc->xnu.temp_queued ||
	    (sc->sassc->flags & MPSSAS_IN_STARTUP) ||
	    !TAILQ_EMPTY(&sc->sassc->ev_queue) ||
	    (sc->xnu.temp_last_us != 0 && now - sc->xnu.temp_last_us <
	    (uint64_t)MPS_XNU_TEMP_PERIOD_S * 1000000))
		return;
	if (mpssas_xnu_queue_work(sc, MPS_XNU_EVENT_TEMP, NULL, 0) == 0)
		sc->xnu.temp_queued = 1;
}

/* MPS_XNU_EVENT_TEMP on the event queue: IO Unit Page 7 */
void
mps_xnu_temperature(struct mps_softc *sc)
{
	Mpi2ConfigReply_t mpi_reply;
	Mpi2IOUnitPage7_t pg;
	struct mps_xnu_stats *st = &sc->xnu.stats;
	int error, t;

	sc->xnu.temp_queued = 0;
	sc->xnu.temp_last_us = mps_os_uptime_us();
	if (mps_xnu_quiet(sc))
		return;
	bzero(&mpi_reply, sizeof(mpi_reply));
	bzero(&pg, sizeof(pg));
	st->temp_reads++;
	error = mps_config_get_iounit_pg7(sc, &mpi_reply, &pg);
	if (error != 0) {
		st->temp_failures++;
		mps_dprint(sc, MPS_INFO, "IO Unit Page 7 read failed (error "
		    "%d: %s, iocstatus 0x%x)\n", error,
		    mps_xnu_errno_str(error), le16toh(mpi_reply.IOCStatus));
		return;
	}
	t = le16toh(pg.IOCTemperature);
	switch (pg.IOCTemperatureUnits) {
	case MPI2_IOUNITPAGE7_IOC_TEMP_CELSIUS:
		break;
	case MPI2_IOUNITPAGE7_IOC_TEMP_FAHRENHEIT:
		t = (t - 32) * 5 / 9;
		break;
	default:			/* NOT_PRESENT */
		sc->xnu.temp_absent = 1;
		st->ioc_temp_valid = 0;
		mps_dprint(sc, MPS_INFO, "IOC temperature: no sensor (IO Unit "
		    "Page 7 units 0x%x), not polled\n",
		    pg.IOCTemperatureUnits);
		return;
	}
	st->ioc_temp_c = t;
	st->ioc_temp_valid = 1;
	if (!sc->xnu.temp_logged) {
		sc->xnu.temp_logged = 1;
		mps_dprint(sc, MPS_INFO, "IOC temperature %d C (IO Unit Page 7, "
		    "polled every %d s)\n", t, MPS_XNU_TEMP_PERIOD_S);
	}
	if (t >= MPS_XNU_TEMP_WARN_C && !sc->xnu.temp_warned) {
		sc->xnu.temp_warned = 1;
		st->temp_warnings++;
		mps_dprint(sc, MPS_ERROR, "IOC temperature %d C: at or above "
		    "%d C, check the enclosure's airflow\n", t,
		    MPS_XNU_TEMP_WARN_C);
	} else if (t < MPS_XNU_TEMP_REARM_C && sc->xnu.temp_warned) {
		sc->xnu.temp_warned = 0;
		mps_dprint(sc, MPS_INFO, "IOC temperature %d C: below %d C "
		    "again\n", t, MPS_XNU_TEMP_REARM_C);
	}
	mps_dprint(sc, MPS_XINFO, "IOC temperature %d C\n", t);
}

void
mps_xnu_config_free(struct mps_softc *sc)
{
	if (sc->xnu.cfg_page != NULL)
		bus_dmamem_free(sc->xnu.cfg_dmat, sc->xnu.cfg_page,
		    sc->xnu.cfg_map);
	if (sc->xnu.cfg_dmat != NULL)
		bus_dma_tag_destroy(sc->xnu.cfg_dmat);
	sc->xnu.cfg_page = NULL;
	sc->xnu.cfg_map = NULL;
	sc->xnu.cfg_dmat = NULL;
	sc->xnu.cfg_busy = 0;
}

int
mps_xnu_target_info(struct mps_softc *sc, u_int tid, uint64_t *sasaddr,
    uint32_t *devinfo, uint8_t *linkrate)
{
	struct mpssas_target *targ;

	if (!mps_xnu_target_present(sc, tid, NULL))
		return (0);
	targ = &sc->sassc->targets[tid];
	*sasaddr = targ->sasaddr;
	*devinfo = targ->devinfo;
	*linkrate = targ->linkrate;
	return (1);
}

/* ---- mps_pci.c counterparts --------------------------------------------- */

int
mps_pci_setup_interrupts(struct mps_softc *sc)
{
	return (mps_os_setup_interrupts(sc->mps_dev));
}

void
mps_pci_free_interrupts(struct mps_softc *sc)
{
	mps_os_free_interrupts(sc->mps_dev);
}

int
mps_pci_restore(struct mps_softc *sc)
{
	return (mps_os_pci_restore(sc->mps_dev));
}
