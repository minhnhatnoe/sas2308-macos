/*-
 * SPDX-License-Identifier: BSD-2-Clause
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

/* ---- logging ----------------------------------------------------------- */

#define MPS_COMPAT_LINE	320

void
mps_compat_vlog(device_t dev, uint32_t cls, const char *fmt, va_list ap)
{
	char buf[MPS_COMPAT_LINE], *line, *nl;

	vsnprintf(buf, sizeof(buf), fmt, ap);
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

/* ---- global lock (mutex registry, intrhooks) ------------------------------ */

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

static struct mtx *mps_compat_mutexes;

void
mtx_init(struct mtx *m, const char *name, const char *type, int opts)
{
	(void)type;
	(void)opts;
	memset(m, 0, sizeof(*m));
	m->mtx_name = name;
	m->mtx_os = mps_os_lock_alloc(name);
	TAILQ_INIT(&m->mtx_callouts);
	mps_compat_global_lock();
	m->mtx_next = mps_compat_mutexes;
	mps_compat_mutexes = m;
	mps_compat_global_unlock();
}

void
mtx_destroy(struct mtx *m)
{
	struct mtx **pp;

	KASSERT(m->mtx_owner == NULL, ("mtx_destroy: %s owned", m->mtx_name));
	KASSERT(TAILQ_EMPTY(&m->mtx_callouts),
	    ("mtx_destroy: %s has armed callouts", m->mtx_name));
	mps_compat_global_lock();
	for (pp = &mps_compat_mutexes; *pp != NULL; pp = &(*pp)->mtx_next) {
		if (*pp == m) {
			*pp = m->mtx_next;
			break;
		}
	}
	mps_compat_global_unlock();
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

void
mtx_unlock(struct mtx *m)
{
	mtx_assert(m, MA_OWNED);
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
	self = mtx->mtx_owner;
	mtx->mtx_owner = NULL;
	error = mps_os_sleep(mtx->mtx_os, chan,
	    timo > 0 ? (uint64_t)timo * 1000 : 0);
	mtx->mtx_owner = self;
	return (error);
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

void
DELAY(int us)
{
	struct mtx *m, *held = NULL;
	void *self;

	if (us <= 0)
		return;
	if (us >= 1000) {
		self = mps_os_curthread();
		mps_compat_global_lock();
		for (m = mps_compat_mutexes; m != NULL; m = m->mtx_next) {
			if (m->mtx_owner == self) {
				held = m;
				break;
			}
		}
		mps_compat_global_unlock();
	}
	if (held != NULL)
		mtx_unlock(held);
	mps_os_delay((uint64_t)us);
	if (held != NULL)
		mtx_lock(held);
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
mps_compat_intrhook_run(void)
{
	struct intr_config_hook *h;

	for (;;) {
		mps_compat_global_lock();
		h = TAILQ_FIRST(&mps_compat_intrhooks);
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
	bus_dma_segment_t *segs;
	int error, maxsegs, nsegs = 0;

	(void)flags;
	maxsegs = dmat->dt_nsegments;
	if (maxsegs <= 0 || maxsegs > MPS_COMPAT_MAXSEGS)
		maxsegs = MPS_COMPAT_MAXSEGS;
	segs = mps_compat_malloc(sizeof(*segs) * maxsegs, NULL,
	    M_NOWAIT | M_ZERO);
	if (segs == NULL)
		return (ENOMEM);
	if (map->dm_cookie == NULL || buf != map->dm_vaddr ||
	    buflen > dmat->dt_maxsize)
		error = EINVAL;		/* XNU: only bus_dmamem_alloc() memory */
	else
		error = mps_os_dmamem_load(dmat, map->dm_cookie, segs, maxsegs,
		    &nsegs);
	if (error == 0)
		map->dm_loaded = 1;
	else
		nsegs = 0;
	callback(callback_arg, segs, nsegs, error);
	mps_compat_free(segs, NULL);
	return (error);
}

void
bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	if (map->dm_cookie != NULL && map->dm_loaded)
		mps_os_dmamem_unload(dmat, map->dm_cookie);
	map->dm_loaded = 0;
}
