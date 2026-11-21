/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_compat.c - shared implementation of the FreeBSD kernel API subset in
 * mps_compat.h, on top of the mps_os_*() platform hooks.
 */
#include "mps_compat.h"

struct thread mps_compat_thread0;

/* ---- strings -------------------------------------------------------------- */

char *
mps_compat_strsep(char **stringp, const char *delim)
{
	char *s, *tok;
	const char *d;

	if ((s = *stringp) == NULL)
		return (NULL);
	for (tok = s; *s != '\0'; s++) {
		for (d = delim; *d != '\0'; d++) {
			if (*s == *d) {
				*s = '\0';
				*stringp = s + 1;
				return (tok);
			}
		}
	}
	*stringp = NULL;
	return (tok);
}

static int
mps_compat_digit(char c, int base)
{
	int v;

	if (c >= '0' && c <= '9')
		v = c - '0';
	else if (c >= 'a' && c <= 'z')
		v = c - 'a' + 10;
	else if (c >= 'A' && c <= 'Z')
		v = c - 'A' + 10;
	else
		return (-1);
	return (v < base ? v : -1);
}

long
mps_compat_strtol(const char *nptr, char **endptr, int base)
{
	const char *s = nptr;
	unsigned long acc = 0;
	int neg = 0, any = 0, d;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '-' || *s == '+')
		neg = (*s++ == '-');
	if ((base == 0 || base == 16) && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X') && mps_compat_digit(s[2], 16) >= 0) {
		s += 2;
		base = 16;
	} else if (base == 0)
		base = (s[0] == '0') ? 8 : 10;
	for (; (d = mps_compat_digit(*s, base)) >= 0; s++) {
		acc = acc * (unsigned long)base + (unsigned long)d;
		any = 1;
	}
	if (endptr != NULL)
		*endptr = __DECONST(char *, any ? s : nptr);
	return (neg ? -(long)acc : (long)acc);
}

int
mps_compat_strcasecmp(const char *a, const char *b)
{
	int ca, cb;

	do {
		ca = (unsigned char)*a++;
		cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
	} while (ca == cb && ca != 0);
	return (ca - cb);
}

size_t
mps_compat_strlcpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);

	if (size != 0) {
		size_t c = (n < size - 1) ? n : size - 1;

		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return (n);
}

/* ---- kernel printf formats ------------------------------------------------ */

/* One conversion of the platform's vsnprintf (va_list: one value at most). */
static int
mps_compat_piece(char *buf, size_t len, const char *spec, ...)
{
	va_list ap;
	int n;

	va_start(ap, spec);
	n = vsnprintf(buf, len, spec, ap);
	va_end(ap);
	return (n);
}

/*
 * FreeBSD subr_prf.c %b: the int in the base the first byte gives (no
 * prefix), then <name,...> of each set bit; a bit byte (1-32) precedes
 * each name.
 */
static int
mps_compat_bits(char *buf, size_t len, u_int v, const char *p)
{
	int n, any = 0, bit;
	size_t used;

	n = mps_compat_piece(buf, len, (*p == 8) ? "%o" : (*p == 10) ? "%u" :
	    "%x", v);
	if (n < 0)
		return (n);
	used = (size_t)n;
	if (v == 0)
		return (n);
	for (p++; (bit = (unsigned char)*p++) != 0; ) {
		if (bit <= 32 && (v & (1u << (bit - 1)))) {
			if (used + 1 < len)
				buf[used] = any ? ',' : '<';
			used++;
			for (; *p > ' '; p++, used++)
				if (used + 1 < len)
					buf[used] = *p;
			any = 1;
		} else
			for (; *p > ' '; p++)
				continue;
	}
	if (any) {
		if (used + 1 < len)
			buf[used] = '>';
		used++;
	}
	if (len != 0)
		buf[used < len ? used : len - 1] = '\0';
	return ((int)used);
}

/*
 * vsnprintf(9) with FreeBSD's %b.  A format without %b goes to the
 * platform's vsnprintf as it is.  Otherwise each conversion is formatted on
 * its own (`*' width/precision resolved first), with the argument type its
 * length modifier names; no floating point (the kernel has none).
 */
int
mps_compat_vsnprintf(char *buf, size_t len, const char *fmt, va_list ap)
{
	char spec[32], tmp[2];
	const char *f, *start;
	size_t used = 0, room;
	char *out;
	int n, sl, lmod;

	for (f = fmt; (f = strchr(f, '%')) != NULL; f++) {
		for (f++; *f != '\0' && strchr("-+ #0123456789.*hljztq", *f);
		    f++)
			continue;
		if (*f == 'b')
			break;
		if (*f == '\0')
			break;
	}
	if (f == NULL || *f != 'b') {
		start = fmt;	/* no %b: a printf(3) format (unchecked copy) */
		return (vsnprintf(buf, len, start, ap));
	}

	for (f = fmt; *f != '\0'; ) {
		room = used < len ? len - used : 0;
		out = room ? buf + used : tmp;
		if (room == 0)
			room = sizeof(tmp);	/* measure only */
		if (*f != '%') {
			start = f;
			while (*f != '\0' && *f != '%')
				f++;
			if (used < len)
				memcpy(buf + used, start, (size_t)(f - start) <
				    len - used ? (size_t)(f - start) : len - used);
			used += (size_t)(f - start);
			continue;
		}
		/* the conversion spec, `*' replaced by the argument */
		start = f++;
		sl = 0;
		spec[sl++] = '%';
		lmod = 0;
		for (; *f != '\0' && strchr("-+ #0123456789.*", *f) != NULL;
		    f++) {
			if (*f == '*')
				sl += mps_compat_piece(spec + sl,
				    sizeof(spec) - (size_t)sl - 8, "%d",
				    va_arg(ap, int));
			else if (sl < (int)sizeof(spec) - 8)
				spec[sl++] = *f;
		}
		for (; *f != '\0' && strchr("hljztq", *f) != NULL; f++) {
			if (sl < (int)sizeof(spec) - 2)
				spec[sl++] = (*f == 'q') ? 'l' : *f;
			lmod = (*f == 'q') ? 'L' :
			    (lmod == 'l' && *f == 'l') ? 'L' :
			    (lmod == 'h' && *f == 'h') ? 'H' : *f;
			if (*f == 'q' && sl < (int)sizeof(spec) - 2)
				spec[sl++] = 'l';
		}
		if (*f == '\0')
			break;
		spec[sl++] = *f;
		spec[sl] = '\0';
		switch (*f++) {
		case 'b': {
			u_int v = va_arg(ap, u_int);
			const char *bits = va_arg(ap, const char *);

			n = mps_compat_bits(out, room, v, bits);
			break;
		}
		case 'd': case 'i':
			if (lmod == 'l')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, long));
			else if (lmod == 'L')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, long long));
			else if (lmod == 'j')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, intmax_t));
			else if (lmod == 'z' || lmod == 't')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, ptrdiff_t));
			else
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, int));
			break;
		case 'o': case 'u': case 'x': case 'X':
			if (lmod == 'l')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, unsigned long));
			else if (lmod == 'L')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, unsigned long long));
			else if (lmod == 'j')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, uintmax_t));
			else if (lmod == 'z' || lmod == 't')
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, size_t));
			else
				n = mps_compat_piece(out, room, spec,
				    va_arg(ap, u_int));
			break;
		case 'c':
			n = mps_compat_piece(out, room, spec, va_arg(ap, int));
			break;
		case 's':
			n = mps_compat_piece(out, room, spec,
			    va_arg(ap, const char *));
			break;
		case 'p':
			n = mps_compat_piece(out, room, spec,
			    va_arg(ap, void *));
			break;
		case '%':
			n = mps_compat_piece(out, room, "%%");
			break;
		default:		/* not a kernel format: printed as is */
			n = (int)(f - start);
			if (out != tmp)
				memcpy(out, start, (size_t)n < room ?
				    (size_t)n : room - 1);
			break;
		}
		if (n < 0)
			return (n);
		used += (size_t)n;
	}
	if (len != 0)
		buf[used < len ? used : len - 1] = '\0';
	return ((int)used);
}

int
mps_compat_snprintf(char *buf, size_t len, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = mps_compat_vsnprintf(buf, len, fmt, ap);
	va_end(ap);
	return (n);
}

/* ---- logging ----------------------------------------------------------- */

#define MPS_COMPAT_LINE	320

void
mps_compat_vlog(device_t dev, uint32_t cls, const char *fmt, va_list ap)
{
	char buf[MPS_COMPAT_LINE], *line, *nl;

	mps_compat_vsnprintf(buf, sizeof(buf), fmt, ap);
	/* One mps_os_log() call per line; empty lines are dropped. */
	for (line = buf; *line != '\0'; line = nl + 1) {
		nl = strchr(line, '\n');
		if (nl != NULL)
			*nl = '\0';
		if (*line != '\0')
			mps_os_log(dev, cls, line);
		if (nl == NULL)
			break;
	}
}

void
mps_compat_log(device_t dev, uint32_t cls, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	mps_compat_vlog(dev, cls, fmt, ap);
	va_end(ap);
}

int
device_printf(device_t dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	mps_compat_vlog(dev, 0, fmt, ap);
	va_end(ap);
	return (0);
}

int
mps_compat_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	mps_compat_vlog(NULL, 0, fmt, ap);
	va_end(ap);
	return (0);
}

void
hexdump(const void *ptr, int length, const char *hdr, int flags)
{
	const uint8_t *p = ptr;
	char line[3 * 16 + 16];
	int i, j, n;

	(void)flags;
	for (i = 0; i < length; i += 16) {
		n = snprintf(line, sizeof(line), "%s%04x ", hdr ? hdr : "", i);
		for (j = i; j < i + 16 && j < length && n < (int)sizeof(line) - 3;
		    j++)
			n += snprintf(line + n, sizeof(line) - n, " %02x", p[j]);
		mps_os_log(NULL, 0, line);
	}
}

/* ---- sbuf(9) -------------------------------------------------------------- */

struct sbuf *
sbuf_new(struct sbuf *s, char *buf, int length, int flags)
{
	KASSERT(s != NULL && buf != NULL && length > 0,
	    ("sbuf_new: fixed buffers only"));
	s->s_buf = buf;
	s->s_size = (size_t)length;
	s->s_len = 0;
	s->s_flags = flags;
	buf[0] = '\0';
	return (s);
}

int
sbuf_vprintf(struct sbuf *s, const char *fmt, va_list ap)
{
	int n;

	if (s->s_len + 1 >= s->s_size)
		return (-1);
	n = vsnprintf(s->s_buf + s->s_len, s->s_size - s->s_len, fmt, ap);
	if (n < 0)
		return (-1);
	s->s_len += (size_t)n;
	if (s->s_len >= s->s_size)
		s->s_len = s->s_size - 1;	/* truncated */
	return (0);
}

int
sbuf_printf(struct sbuf *s, const char *fmt, ...)
{
	va_list ap;
	int error;

	va_start(ap, fmt);
	error = sbuf_vprintf(s, fmt, ap);
	va_end(ap);
	return (error);
}

int
sbuf_cat(struct sbuf *s, const char *str)
{
	return (sbuf_printf(s, "%s", str));
}

int
sbuf_finish(struct sbuf *s)
{
	s->s_buf[s->s_len] = '\0';
	return (0);
}

char *
sbuf_data(struct sbuf *s)
{
	return (s->s_buf);
}

void
sbuf_delete(struct sbuf *s)
{
	(void)s;
}

/* ---- KASSERT / panic ------------------------------------------------------ */

void
mps_compat_kassert_where(const char *file, int line, const char *expr)
{
	mps_compat_log(NULL, 0, "KASSERT failed: %s at %s:%d", expr, file,
	    line);
}

void
mps_compat_kassert(const char *fmt, ...)
{
	char buf[MPS_COMPAT_LINE];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	mps_os_assert_fail(buf);
}

void
mps_compat_panic(const char *fmt, ...)
{
	char buf[MPS_COMPAT_LINE];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	mps_compat_log(NULL, 0, "panic: %s", buf);
	mps_os_assert_fail(buf);
}

/* ---- device ------------------------------------------------------------- */

void
mps_compat_device_init(device_t dev, const char *name, int unit, void *softc,
    void *os, uint16_t devid)
{
	memset(dev, 0, sizeof(*dev));
	dev->dv_name = name;
	dev->dv_unit = unit;
	snprintf(dev->dv_nameunit, sizeof(dev->dv_nameunit), "%s%d", name,
	    unit);
	dev->dv_softc = softc;
	dev->dv_os = os;
	dev->dv_devid = devid;
	dev->dv_dmat.dt_dev = dev;
	dev->dv_dmat.dt_alignment = 1;
	dev->dv_dmat.dt_lowaddr = BUS_SPACE_MAXADDR;
	dev->dv_dmat.dt_highaddr = BUS_SPACE_MAXADDR;
	dev->dv_dmat.dt_maxsize = BUS_SPACE_MAXSIZE;
	dev->dv_dmat.dt_nsegments = BUS_SPACE_UNRESTRICTED;
	dev->dv_dmat.dt_maxsegsz = BUS_SPACE_MAXSIZE;
}

/* ---- global lock (intrhooks) ---------------------------------------------- */

static void *mps_compat_glock;

static void
mps_compat_global_lock(void)
{
	void *l, *expect = NULL;

	if ((l = __atomic_load_n(&mps_compat_glock, __ATOMIC_ACQUIRE)) == NULL) {
		l = mps_os_lock_alloc("mps_compat");
		if (!__atomic_compare_exchange_n(&mps_compat_glock, &expect, l,
		    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			mps_os_lock_free(l);
			l = expect;
		}
	}
	mps_os_lock(l);
}

static void
mps_compat_global_unlock(void)
{
	mps_os_unlock(mps_compat_glock);
}

/* ---- malloc(9) -------------------------------------------------------------- */

static long mps_compat_mallocs;

struct mps_malloc_hdr {
	size_t	size;
	size_t	magic;
};
#define MPS_MALLOC_MAGIC	0x6d70735f6d616cUL

void *
mps_compat_malloc(size_t size, struct malloc_type *type, int flags)
{
	struct mps_malloc_hdr *h;

	(void)type;
	h = mps_os_malloc(sizeof(*h) + size, (flags & M_WAITOK) != 0);
	if (h == NULL)
		return (NULL);
	h->size = size;
	h->magic = MPS_MALLOC_MAGIC;
	if (flags & M_ZERO)
		memset(h + 1, 0, size);
	__atomic_add_fetch(&mps_compat_mallocs, 1, __ATOMIC_RELAXED);
	return (h + 1);
}

void
mps_compat_free(void *addr, struct malloc_type *type)
{
	struct mps_malloc_hdr *h;

	(void)type;
	if (addr == NULL)
		return;
	h = (struct mps_malloc_hdr *)addr - 1;
	KASSERT(h->magic == MPS_MALLOC_MAGIC, ("free: bad pointer %p", addr));
	h->magic = 0;
	__atomic_sub_fetch(&mps_compat_mallocs, 1, __ATOMIC_RELAXED);
	mps_os_free(h, sizeof(*h) + h->size);
}

long
mps_compat_malloc_count(void)
{
	return (__atomic_load_n(&mps_compat_mallocs, __ATOMIC_RELAXED));
}

/* ---- time ------------------------------------------------------------------- */

uint64_t
mps_compat_ticks(void)
{
	return (mps_os_uptime_us() / 1000);
}

void
timevalsub(struct timeval *t1, const struct timeval *t2)
{
	t1->tv_sec -= t2->tv_sec;
	t1->tv_usec -= t2->tv_usec;
	if (t1->tv_usec < 0) {
		t1->tv_sec--;
		t1->tv_usec += 1000000;
	}
}

void
getmicrouptime(struct timeval *tv)
{
	uint64_t us = mps_os_uptime_us();

	tv->tv_sec = (long)(us / 1000000);
	tv->tv_usec = (long)(us % 1000000);
}

void
getmicrotime(struct timeval *tv)
{
	uint64_t sec;
	uint32_t usec;

	mps_os_walltime(&sec, &usec);
	tv->tv_sec = (long)sec;
	tv->tv_usec = (long)usec;
}

sbintime_t
sbinuptime(void)
{
	uint64_t us = mps_os_uptime_us();

	return ((sbintime_t)(us / 1000000) * SBT_1S +
	    (sbintime_t)(us % 1000000) * SBT_1US);
}

/* FreeBSD ratecheck(9): at most one event per `mininterval'. */
int
ratecheck(struct timeval *lasttime, const struct timeval *mininterval)
{
	struct timeval tv, delta;

	getmicrouptime(&tv);
	delta = tv;
	timevalsub(&delta, lasttime);
	if ((lasttime->tv_sec == 0 && lasttime->tv_usec == 0) ||
	    delta.tv_sec > mininterval->tv_sec ||
	    (delta.tv_sec == mininterval->tv_sec &&
	    delta.tv_usec >= mininterval->tv_usec)) {
		*lasttime = tv;
		return (1);
	}
	return (0);
}

/* ---- mutex(9), sleep(9) ------------------------------------------------------ */

void
mtx_init(struct mtx *m, const char *name, const char *type, int opts)
{
	(void)type;
	(void)opts;
	memset(m, 0, sizeof(*m));
	m->mtx_name = name;
	m->mtx_os = mps_os_lock_alloc(name);
	TAILQ_INIT(&m->mtx_callouts);
}

void
mtx_destroy(struct mtx *m)
{
	KASSERT(m->mtx_owner == NULL, ("mtx_destroy: %s owned", m->mtx_name));
	KASSERT(TAILQ_EMPTY(&m->mtx_callouts),
	    ("mtx_destroy: %s has armed callouts", m->mtx_name));
	if (m->mtx_armed_us != 0)
		mps_os_callout_arm(m, 0);
	mps_os_lock_free(m->mtx_os);
	m->mtx_os = NULL;
}

void
mtx_lock(struct mtx *m)
{
	KASSERT(!mtx_owned(m), ("mtx_lock: %s recursed", m->mtx_name));
	mps_os_lock(m->mtx_os);
	m->mtx_owner = mps_os_curthread();
}

static void
mtx_release_work(struct mtx *m)
{
	while (m->mtx_release_pending) {
		m->mtx_release_pending = 0;
		m->mtx_release_fn(m->mtx_release_arg);
	}
}

void
mtx_unlock(struct mtx *m)
{
	mtx_assert(m, MA_OWNED);
	mtx_release_work(m);
	m->mtx_owner = NULL;
	mps_os_unlock(m->mtx_os);
}

int
msleep(void *chan, struct mtx *mtx, int pri, const char *wmesg, int timo)
{
	void *self;
	int error;

	(void)pri;
	(void)wmesg;
	mtx_assert(mtx, MA_OWNED);
	mtx_release_work(mtx);
	self = mtx->mtx_owner;
	mtx->mtx_owner = NULL;
	error = mps_os_sleep(mtx->mtx_os, chan,
	    timo > 0 ? (uint64_t)timo * 1000 : 0);
	mtx->mtx_owner = self;
	/* the hook's errno space may differ (Linux host EWOULDBLOCK = 11) */
	return (error != 0 ? EWOULDBLOCK : 0);
}

void
wakeup(void *chan)
{
	mps_os_wakeup(chan);
}

void
pause(const char *wmesg, int timo)
{
	(void)wmesg;
	mps_os_delay((uint64_t)(timo > 0 ? timo : 1) * 1000);
}

/*
 * As FreeBSD's DELAY(9), the caller's locks stay held: every FreeBSD caller
 * that holds mps_mtx across it (the doorbell handshake in mps_wait_db_int,
 * mps_transition_ready, the NO_SLEEP branches of the diag reset) relies on
 * nothing else running in between.  The platform may sleep for 1 ms and
 * more instead of spinning (lck_mtx may be held across a sleep); the long
 * waits FreeBSD writes as msleep()/pause() drop the lock as there.
 */
void
DELAY(int us)
{
	if (us <= 0)
		return;
	mps_os_delay((uint64_t)us);
}

/* ---- callout(9) ----------------------------------------------------------- */

static void
callout_rearm(struct mtx *m)
{
	struct callout *c;
	uint64_t when;

	c = TAILQ_FIRST(&m->mtx_callouts);
	when = (c != NULL) ? c->c_time_us : 0;
	if (when != m->mtx_armed_us) {
		m->mtx_armed_us = when;
		mps_os_callout_arm(m, when);
	}
}

void
callout_init_mtx(struct callout *c, struct mtx *mtx, int flags)
{
	(void)flags;
	memset(c, 0, sizeof(*c));
	c->c_mtx = mtx;
}

static int
callout_remove(struct callout *c)
{
	if ((c->c_flags & CALLOUT_PENDING) == 0)
		return (0);
	TAILQ_REMOVE(&c->c_mtx->mtx_callouts, c, c_link);
	c->c_flags &= ~CALLOUT_PENDING;
	return (1);
}

static int
callout_reset_us(struct callout *c, uint64_t delta_us, void (*func)(void *),
    void *arg)
{
	struct callout *it;
	struct mtx *m = c->c_mtx;
	int cancelled, locked;

	/* FreeBSD's mps_attach() arms the watchdog without mps_mtx. */
	locked = !mtx_owned(m);
	if (locked)
		mtx_lock(m);
	cancelled = callout_remove(c);
	c->c_time_us = mps_os_uptime_us() + (delta_us ? delta_us : 1000);
	c->c_func = func;
	c->c_arg = arg;
	c->c_flags |= CALLOUT_PENDING;
	TAILQ_FOREACH(it, &m->mtx_callouts, c_link) {
		if (it->c_time_us > c->c_time_us)
			break;
	}
	if (it != NULL)
		TAILQ_INSERT_BEFORE(it, c, c_link);
	else
		TAILQ_INSERT_TAIL(&m->mtx_callouts, c, c_link);
	callout_rearm(m);
	if (locked)
		mtx_unlock(m);
	return (cancelled);
}

int
callout_reset(struct callout *c, int to_ticks, void (*func)(void *),
    void *arg)
{
	return (callout_reset_us(c,
	    (uint64_t)(to_ticks > 0 ? to_ticks : 1) * 1000, func, arg));
}

int
callout_reset_sbt(struct callout *c, sbintime_t sbt, sbintime_t pr,
    void (*func)(void *), void *arg, int flags)
{
	(void)pr;
	(void)flags;
	return (callout_reset_us(c, (uint64_t)(sbt / SBT_1US), func, arg));
}

int
callout_stop(struct callout *c)
{
	int cancelled, locked;

	if (c->c_mtx == NULL)
		return (0);
	locked = !mtx_owned(c->c_mtx);
	if (locked)
		mtx_lock(c->c_mtx);
	cancelled = callout_remove(c);
	callout_rearm(c->c_mtx);
	if (locked)
		mtx_unlock(c->c_mtx);
	return (cancelled);
}

/*
 * Callouts run with their mutex held (mps_compat_callout_run), so once the
 * mutex is ours the callout is not running.
 */
int
callout_drain(struct callout *c)
{
	return (callout_stop(c));
}

void
mps_compat_callout_run(struct mtx *m)
{
	struct callout *c;
	void (*func)(void *);
	void *arg;

	mtx_lock(m);
	m->mtx_armed_us = 0;
	while ((c = TAILQ_FIRST(&m->mtx_callouts)) != NULL &&
	    c->c_time_us <= mps_os_uptime_us()) {
		callout_remove(c);
		func = c->c_func;
		arg = c->c_arg;
		func(arg);
	}
	callout_rearm(m);
	mtx_unlock(m);
}

/* ---- taskqueue(9) ---------------------------------------------------------- */

struct taskqueue *
taskqueue_create(const char *name, int mflags, taskqueue_enqueue_fn *enqueue,
    void *context)
{
	struct taskqueue *tq;

	(void)enqueue;
	(void)context;
	tq = mps_compat_malloc(sizeof(*tq), NULL, (mflags & M_WAITOK) | M_ZERO);
	if (tq == NULL)
		return (NULL);
	TAILQ_INIT(&tq->tq_queue);
	tq->tq_name = name;
	tq->tq_lock = mps_os_lock_alloc(name);
	if (tq->tq_lock == NULL) {
		mps_compat_free(tq, NULL);
		return (NULL);
	}
	return (tq);
}

int
taskqueue_start_threads(struct taskqueue **tqp, int count, int pri,
    const char *name, ...)
{
	(void)count;
	(void)pri;
	(void)name;
	if (*tqp == NULL)
		return (EINVAL);
	return (mps_os_taskqueue_start(*tqp));
}

int
taskqueue_enqueue(struct taskqueue *tq, struct task *task)
{
	mps_os_lock(tq->tq_lock);
	if (task->ta_pending != 0) {
		if (task->ta_pending < 0xffff)
			task->ta_pending++;
		mps_os_unlock(tq->tq_lock);
		return (0);
	}
	TAILQ_INSERT_TAIL(&tq->tq_queue, task, ta_link);
	task->ta_pending = 1;
	mps_os_unlock(tq->tq_lock);
	mps_os_taskqueue_kick(tq);
	return (0);
}

void
mps_compat_taskqueue_run(struct taskqueue *tq)
{
	struct task *task;
	int pending;

	mps_os_lock(tq->tq_lock);
	/*
	 * FreeBSD runs every mps queue on one thread (taskqueue_start_threads(
	 * ..., 1, ...)), so its tasks never overlap: mpssas_firmware_event_work
	 * handles one firmware event at a time even while it sleeps in a config
	 * page read.  A platform thread call kicked while it runs starts a
	 * second invocation on another thread (xnu thread_call): that one
	 * leaves the queue to the running one, whose loop picks up the task.
	 */
	if (tq->tq_active) {
		mps_os_unlock(tq->tq_lock);
		return;
	}
	tq->tq_active = 1;
	while ((task = TAILQ_FIRST(&tq->tq_queue)) != NULL) {
		TAILQ_REMOVE(&tq->tq_queue, task, ta_link);
		pending = task->ta_pending;
		task->ta_pending = 0;
		tq->tq_running = task;
		mps_os_unlock(tq->tq_lock);
		task->ta_func(task->ta_context, pending);
		mps_os_lock(tq->tq_lock);
		tq->tq_running = NULL;
	}
	tq->tq_active = 0;
	mps_os_unlock(tq->tq_lock);
}

void
taskqueue_drain(struct taskqueue *tq, struct task *task)
{
	mps_os_lock(tq->tq_lock);
	while (task->ta_pending != 0 || tq->tq_running == task) {
		mps_os_unlock(tq->tq_lock);
		mps_os_taskqueue_wait(tq);
		mps_os_lock(tq->tq_lock);
	}
	mps_os_unlock(tq->tq_lock);
}

/* As FreeBSD: tasks still queued run before the queue goes away. */
void
taskqueue_free(struct taskqueue *tq)
{
	tq->tq_flags |= TQ_FLAGS_STOPPED;
	mps_os_taskqueue_stop(tq);
	mps_compat_taskqueue_run(tq);
	mps_os_lock_free(tq->tq_lock);
	mps_compat_free(tq, NULL);
}

/* ---- config_intrhook(9) ---------------------------------------------------- */

static TAILQ_HEAD(, intr_config_hook) mps_compat_intrhooks =
    TAILQ_HEAD_INITIALIZER(mps_compat_intrhooks);

int
config_intrhook_establish(struct intr_config_hook *hook)
{
	struct intr_config_hook *h;

	mps_compat_global_lock();
	TAILQ_FOREACH(h, &mps_compat_intrhooks, ich_links) {
		if (h == hook) {
			mps_compat_global_unlock();
			return (EEXIST);
		}
	}
	TAILQ_INSERT_TAIL(&mps_compat_intrhooks, hook, ich_links);
	mps_compat_global_unlock();
	return (0);
}

void
config_intrhook_disestablish(struct intr_config_hook *hook)
{
	struct intr_config_hook *h;

	mps_compat_global_lock();
	TAILQ_FOREACH(h, &mps_compat_intrhooks, ich_links) {
		if (h == hook) {
			TAILQ_REMOVE(&mps_compat_intrhooks, hook, ich_links);
			break;
		}
	}
	mps_compat_global_unlock();
}

void
mps_compat_intrhook_run(void *arg)
{
	struct intr_config_hook *h;

	for (;;) {
		mps_compat_global_lock();
		TAILQ_FOREACH(h, &mps_compat_intrhooks, ich_links)
			if (h->ich_arg == arg)
				break;
		if (h != NULL)
			TAILQ_REMOVE(&mps_compat_intrhooks, h, ich_links);
		mps_compat_global_unlock();
		if (h == NULL)
			break;
		/* ich_func calls config_intrhook_disestablish(): a no-op now */
		h->ich_func(h->ich_arg);
	}
}

/* ---- bus_dma(9) -------------------------------------------------------------- */

struct bus_dmamap {
	void		*dm_cookie;	/* mps_os_dmamem_alloc(), NULL otherwise */
	void		*dm_vaddr;
	int		 dm_loaded;
	bus_dmamap_t	 dm_lent;	/* bus_dmamap_xnu_lend() memory, until unload */
};

static long mps_compat_dmamems;

void
bus_dma_template_init(bus_dma_template_t *t, bus_dma_tag_t parent)
{
	memset(t, 0, sizeof(*t));
	t->dt_dev = (parent != NULL) ? parent->dt_dev : NULL;
	t->dt_alignment = 1;
	t->dt_boundary = 0;
	t->dt_lowaddr = BUS_SPACE_MAXADDR;
	t->dt_highaddr = BUS_SPACE_MAXADDR;
	t->dt_maxsize = BUS_SPACE_MAXSIZE;
	t->dt_nsegments = BUS_SPACE_UNRESTRICTED;
	t->dt_maxsegsz = BUS_SPACE_MAXSIZE;
	/* The parent's restrictions are inherited (bus_dma_tag_create). */
	if (parent != NULL) {
		t->dt_alignment = parent->dt_alignment;
		t->dt_boundary = parent->dt_boundary;
		t->dt_lowaddr = parent->dt_lowaddr;
	}
}

void
bus_dma_template_fill(bus_dma_template_t *t, bus_dma_param_t *kv, u_int count)
{
	u_int i;

	for (i = 0; i < count; i++) {
		switch (kv[i].id) {
		case BD_PARAM_ALIGNMENT:
			if (kv[i].num > t->dt_alignment)
				t->dt_alignment = kv[i].num;
			break;
		case BD_PARAM_BOUNDARY:
			/* the parent's, or the smaller of two (tag_create) */
			if (t->dt_boundary == 0 || (kv[i].num != 0 &&
			    kv[i].num < t->dt_boundary))
				t->dt_boundary = kv[i].num;
			break;
		case BD_PARAM_LOWADDR:
			if (kv[i].num < t->dt_lowaddr)
				t->dt_lowaddr = kv[i].num;
			break;
		case BD_PARAM_HIGHADDR:
			t->dt_highaddr = kv[i].num;
			break;
		case BD_PARAM_MAXSIZE:
			t->dt_maxsize = kv[i].num;
			break;
		case BD_PARAM_NSEGMENTS:
			t->dt_nsegments = (int)kv[i].num;
			break;
		case BD_PARAM_MAXSEGSIZE:
			t->dt_maxsegsz = kv[i].num;
			break;
		case BD_PARAM_FLAGS:
			t->dt_flags = (int)kv[i].num;
			break;
		default:
			break;
		}
	}
}

int
bus_dma_template_tag(bus_dma_template_t *t, bus_dma_tag_t *dmat)
{
	bus_dma_tag_t tag;

	tag = mps_compat_malloc(sizeof(*tag), NULL, M_NOWAIT);
	if (tag == NULL)
		return (ENOMEM);
	*tag = *t;
	if (tag->dt_nsegments <= 0)
		tag->dt_nsegments = BUS_SPACE_UNRESTRICTED;
	*dmat = tag;
	return (0);
}

void
bus_dma_template_clone(bus_dma_template_t *t, bus_dma_tag_t dmat)
{
	*t = *dmat;
}

int
bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
    bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
    void *filter, void *filterarg, bus_size_t maxsize, int nsegments,
    bus_size_t maxsegsz, int flags, bus_dma_lock_t *lockfunc,
    void *lockfuncarg, bus_dma_tag_t *dmat)
{
	bus_dma_template_t t;

	(void)filter;
	(void)filterarg;
	(void)lockfunc;
	(void)lockfuncarg;
	bus_dma_template_init(&t, parent);
	BUS_DMA_TEMPLATE_FILL(&t, BD_ALIGNMENT(alignment),
	    BD_BOUNDARY(boundary), BD_LOWADDR(lowaddr), BD_HIGHADDR(highaddr),
	    BD_MAXSIZE(maxsize), BD_NSEGMENTS(nsegments),
	    BD_MAXSEGSIZE(maxsegsz), BD_FLAGS(flags));
	return (bus_dma_template_tag(&t, dmat));
}

int
bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
	mps_compat_free(dmat, NULL);
	return (0);
}

int
bus_dmamem_alloc(bus_dma_tag_t dmat, void **vaddr, int flags,
    bus_dmamap_t *mapp)
{
	bus_dmamap_t map;
	int error;

	map = mps_compat_malloc(sizeof(*map), NULL, M_NOWAIT | M_ZERO);
	if (map == NULL)
		return (ENOMEM);
	error = mps_os_dmamem_alloc(dmat, &map->dm_vaddr, &map->dm_cookie);
	if (error != 0 || map->dm_vaddr == NULL) {
		mps_compat_free(map, NULL);
		return (error != 0 ? error : ENOMEM);
	}
	if (flags & BUS_DMA_ZERO)
		memset(map->dm_vaddr, 0, (size_t)dmat->dt_maxsize);
	__atomic_add_fetch(&mps_compat_dmamems, 1, __ATOMIC_RELAXED);
	*vaddr = map->dm_vaddr;
	*mapp = map;
	return (0);
}

void
bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map)
{
	KASSERT(map != NULL && map->dm_vaddr == vaddr,
	    ("bus_dmamem_free: bad map"));
	if (map->dm_loaded)
		bus_dmamap_unload(dmat, map);
	mps_os_dmamem_free(dmat, vaddr, map->dm_cookie);
	__atomic_sub_fetch(&mps_compat_dmamems, 1, __ATOMIC_RELAXED);
	mps_compat_free(map, NULL);
}

long
mps_compat_dmamem_count(void)
{
	return (__atomic_load_n(&mps_compat_dmamems, __ATOMIC_RELAXED));
}

int
bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp)
{
	(void)dmat;
	(void)flags;
	*mapp = mps_compat_malloc(sizeof(**mapp), NULL, M_NOWAIT | M_ZERO);
	return (*mapp == NULL ? ENOMEM : 0);
}

int
bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	(void)dmat;
	mps_compat_free(map, NULL);
	return (0);
}

#define MPS_COMPAT_MAXSEGS	4096

int
bus_dmamap_load(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf,
    bus_size_t buflen, bus_dmamap_callback_t *callback, void *callback_arg,
    int flags)
{
	bus_dma_segment_t *segs, few[16];
	bus_dmamap_t mem;
	bus_size_t left;
	int error, i, maxsegs, nsegs = 0;

	(void)flags;
	maxsegs = dmat->dt_nsegments;
	if (maxsegs <= 0 || maxsegs > MPS_COMPAT_MAXSEGS)
		maxsegs = MPS_COMPAT_MAXSEGS;
	/*
	 * XNU: the queue, reply, request and sense loads ignore the result
	 * and their callbacks read segs[0] (FreeBSD's never fail): no
	 * allocation for a short list, so they cannot fail here.
	 */
	if (maxsegs <= (int)nitems(few)) {
		segs = few;
		bzero(few, sizeof(few));
	} else {
		segs = mps_compat_malloc(sizeof(*segs) * maxsegs, NULL,
		    M_NOWAIT | M_ZERO);
		if (segs == NULL) {
			bzero(few, sizeof(few));
			callback(callback_arg, few, 0, ENOMEM);
			return (ENOMEM);
		}
	}
	mem = (map->dm_lent != NULL) ? map->dm_lent : map;
	if (mem->dm_cookie == NULL || buf != mem->dm_vaddr ||
	    buflen > dmat->dt_maxsize)
		error = EINVAL;		/* XNU: only bus_dmamem_alloc() memory */
	else
		error = mps_os_dmamem_load(dmat, mem->dm_cookie, segs, maxsegs,
		    &nsegs);
	if (error == 0) {
		/* the list covers the allocation: cut it to `buflen' */
		for (i = 0, left = buflen; i < nsegs && left > 0; i++) {
			if (segs[i].ds_len > left)
				segs[i].ds_len = left;
			left -= segs[i].ds_len;
		}
		nsegs = i;
		map->dm_loaded = 1;
	} else
		nsegs = 0;
	callback(callback_arg, segs, nsegs, error);
	if (segs != few)
		mps_compat_free(segs, NULL);
	/*
	 * busdma's contract: the callback has the error; only ENOMEM is also
	 * returned (bus_dmamap_load_mem), so a caller never handles it twice.
	 */
	return (error == ENOMEM ? ENOMEM : 0);
}

/*
 * XNU: lend `mem' (from bus_dmamem_alloc()) to `map' until map's next
 * bus_dmamap_unload(), so bus_dmamap_load(map, mem's buffer) works on a
 * command's own map.  FreeBSD loads any kernel buffer; here only
 * bus_dmamem_alloc() memory has a bus address (mps_config.c uses this).
 */
void
bus_dmamap_xnu_lend(bus_dmamap_t map, bus_dmamap_t mem)
{
	KASSERT(map->dm_lent == NULL && mem->dm_cookie != NULL,
	    ("bus_dmamap_xnu_lend: map busy or not dmamem"));
	map->dm_lent = mem;
}

void
bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	bus_dmamap_t mem = (map->dm_lent != NULL) ? map->dm_lent : map;

	if (mem->dm_cookie != NULL && map->dm_loaded)
		mps_os_dmamem_unload(dmat, mem->dm_cookie);
	map->dm_loaded = 0;
	map->dm_lent = NULL;
}
