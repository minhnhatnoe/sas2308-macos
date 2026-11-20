/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * mps_compat.h - the subset of the FreeBSD kernel API that the ported
 * sys/dev/mps code (mps.c, mps_sas.c, mps_sas_lsi.c, mps_table.c) uses.
 *
 * Shared implementation in mps_compat.c; everything platform specific goes
 * through the mps_os_*() hooks declared at the end of this file (one
 * implementation in the kext, kext/mps_xnu_os.cpp, one in the host test,
 * test/mps_host_os.cpp).  See docs/design.md "Compat layer contract".
 *
 * Plain C11.  Only <stdint.h>, <stddef.h>, <stdarg.h>, <string.h> and, in the
 * kernel, <libkern/libkern.h> are included, so the same file builds with the
 * kext flags (-nostdinc, Kernel.framework headers) and as host C/C++.
 *
 * Define MPS_COMPAT_NO_LIBC_REMAP before including this header (host test
 * code) to keep malloc()/free()/printf() as the C library's.
 */
#ifndef _MPS_COMPAT_H_
#define _MPS_COMPAT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#if defined(KERNEL)
#include <libkern/libkern.h>	/* snprintf, vsnprintf */
#else
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "mps core: little-endian hosts only (arm64, x86_64)"
#endif

/* ---- <sys/cdefs.h>, <sys/types.h>, <sys/param.h> ------------------------ */

#ifndef __unused
#define __unused		__attribute__((__unused__))
#endif
#ifndef __printflike
#define __printflike(f, a)	__attribute__((__format__(__printf__, f, a)))
#endif
#ifndef __predict_false
#define __predict_false(x)	__builtin_expect(!!(x), 0)
#define __predict_true(x)	__builtin_expect(!!(x), 1)
#endif
#ifndef __containerof
#define __containerof(x, s, m)	((s *)(void *)((char *)(x) - offsetof(s, m)))
#endif
#ifndef __DECONST
#define __DECONST(type, var)	((type)(uintptr_t)(const void *)(var))
#endif

#include "queue.h"

typedef unsigned char	u_char;
typedef unsigned short	u_short;
typedef unsigned int	u_int;
typedef unsigned long	u_long;
typedef uint64_t	bus_addr_t;
typedef uint64_t	bus_size_t;
typedef uint64_t	vm_paddr_t;
typedef uintptr_t	bus_space_tag_t;
typedef uintptr_t	bus_space_handle_t;
typedef int64_t		sbintime_t;

#ifndef TRUE
#define TRUE	1
#define FALSE	0
#endif

#undef MIN
#undef MAX
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#if !defined(KERNEL)	/* Kernel.framework <libkern/libkern.h> has the same */
static __inline u_int min(u_int a, u_int b) { return (a < b ? a : b); }
static __inline u_int max(u_int a, u_int b) { return (a > b ? a : b); }
#endif
#undef roundup2
#undef howmany
#undef nitems
#define roundup2(x, y)	(((x) + ((y) - 1)) & (~((__typeof(x))(y) - 1)))
#define howmany(x, y)	(((x) + ((y) - 1)) / (y))
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#undef NBBY
#define NBBY		8
#define setbit(a, i)	(((unsigned char *)(a))[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i)	(((unsigned char *)(a))[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i)	(((const unsigned char *)(a))[(i) / NBBY] & (1 << ((i) % NBBY)))

/*
 * XNU: the core's page size is 4 KiB (MPI2 SGE sizing, FreeBSD maxio
 * arithmetic), independent of the 16 KiB arm64 kernel page.
 */
#undef PAGE_SIZE
#define PAGE_SIZE	4096
#define maxphys		(1024 * 1024)
#define mp_ncpus	1
#define bootverbose	0

/* errno values (FreeBSD numbering; only compared inside the core) */
#ifndef EPERM
#define EPERM		1
#endif
#ifndef ENOENT
#define ENOENT		2
#endif
#ifndef EIO
#define EIO		5
#endif
#ifndef ENXIO
#define ENXIO		6
#endif
#ifndef ENOMEM
#define ENOMEM		12
#endif
#ifndef EFAULT
#define EFAULT		14
#endif
#ifndef EBUSY
#define EBUSY		16
#endif
#ifndef EEXIST
#define EEXIST		17
#endif
#ifndef ENODEV
#define ENODEV		19
#endif
#ifndef EINVAL
#define EINVAL		22
#endif
#ifndef EFBIG
#define EFBIG		27
#endif
#ifndef EAGAIN
#define EAGAIN		35
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK	EAGAIN
#endif
#ifndef EINPROGRESS
#define EINPROGRESS	36
#endif
#ifndef ENOBUFS
#define ENOBUFS		55
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT	60
#endif

/* ---- <sys/endian.h>: little-endian only (checked above) ----------------- */

#undef htole16
#undef htole32
#undef htole64
#undef le16toh
#undef le32toh
#undef le64toh
#define htole16(x)	((uint16_t)(x))
#define htole32(x)	((uint32_t)(x))
#define htole64(x)	((uint64_t)(x))
#define le16toh(x)	((uint16_t)(x))
#define le32toh(x)	((uint32_t)(x))
#define le64toh(x)	((uint64_t)(x))

/* ---- <machine/atomic.h> ------------------------------------------------- */

#define atomic_swap_64(p, v)	__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

/* ---- libkern / string ---------------------------------------------------- */

#ifndef MPS_COMPAT_NO_LIBC_REMAP
#define bzero(p, n)		memset((p), 0, (n))
#define bcopy(s, d, n)		memmove((d), (s), (n))
#define strsep			mps_compat_strsep
#define strtol			mps_compat_strtol
#define strcasecmp		mps_compat_strcasecmp
#define strlcpy			mps_compat_strlcpy
#endif
char	*mps_compat_strsep(char **stringp, const char *delim);
long	 mps_compat_strtol(const char *nptr, char **endptr, int base);
int	 mps_compat_strcasecmp(const char *a, const char *b);
size_t	 mps_compat_strlcpy(char *dst, const char *src, size_t size);

/* ---- KASSERT / panic ----------------------------------------------------- */

void	mps_compat_kassert_where(const char *file, int line, const char *expr);
void	mps_compat_kassert(const char *fmt, ...) __printflike(1, 2);
void	mps_compat_panic(const char *fmt, ...) __printflike(1, 2);

#define KASSERT(exp, msg) do {						\
	if (__predict_false(!(exp))) {					\
		mps_compat_kassert_where(__FILE__, __LINE__, #exp);	\
		mps_compat_kassert msg;					\
	}								\
} while (0)
#define MPASS(exp)	KASSERT((exp), ("%s", #exp))
#ifndef MPS_COMPAT_NO_LIBC_REMAP
#define panic		mps_compat_panic
#endif

/* ---- device_t, printf(9) ------------------------------------------------- */

struct mps_softc;

/* Root DMA constraints of a device; tags inherit dt_dev from it. */
struct device;
typedef struct device *device_t;

typedef int bus_dma_lock_t(void *, int);
typedef struct bus_dma_tag *bus_dma_tag_t;
typedef struct bus_dmamap *bus_dmamap_t;

struct bus_dma_tag {
	device_t	dt_dev;		/* owner, for the mps_os_dma hooks */
	bus_size_t	dt_alignment;
	bus_addr_t	dt_boundary;
	bus_addr_t	dt_lowaddr;	/* highest address the device can reach */
	bus_addr_t	dt_highaddr;
	bus_size_t	dt_maxsize;
	int		dt_nsegments;
	bus_size_t	dt_maxsegsz;
	int		dt_flags;
};

struct device {
	const char	*dv_name;	/* "mps" */
	int		 dv_unit;
	char		 dv_nameunit[16];
	void		*dv_softc;
	void		*dv_os;		/* platform object (kext: the IOService) */
	uint16_t	 dv_devid;	/* PCI device ID */
	struct bus_dma_tag dv_dmat;	/* root tag: bus_get_dma_tag() */
};

void	 mps_compat_device_init(device_t dev, const char *name, int unit,
	    void *softc, void *os, uint16_t devid);
#define device_get_unit(dev)		((dev)->dv_unit)
#define device_get_nameunit(dev)	((const char *)(dev)->dv_nameunit)
#define device_get_softc(dev)		((dev)->dv_softc)
#define pci_get_device(dev)		((dev)->dv_devid)
#define bus_get_dma_tag(dev)		(&(dev)->dv_dmat)

/* Every line ends up in mps_os_log(dev, cls, line), one call per line. */
void	mps_compat_log(device_t dev, uint32_t cls, const char *fmt, ...)
	    __printflike(3, 4);
void	mps_compat_vlog(device_t dev, uint32_t cls, const char *fmt,
	    va_list ap) __printflike(3, 0);
int	device_printf(device_t dev, const char *fmt, ...) __printflike(2, 3);
int	mps_compat_printf(const char *fmt, ...) __printflike(1, 2);
void	hexdump(const void *ptr, int length, const char *hdr, int flags);
#ifndef MPS_COMPAT_NO_LIBC_REMAP
#define printf		mps_compat_printf
#endif

/* Never used by the port; keep struct mps_softc verbatim. */
struct sysctl_ctx_list {
	int	xnu_unused;
};
typedef void *eventhandler_tag;

/* ---- sbuf(9): fixed-length buffers only ------------------------------------ */

struct sbuf {
	char	*s_buf;
	size_t	 s_size;
	size_t	 s_len;
	int	 s_flags;
};
#define SBUF_FIXEDLEN	0x00000000

struct sbuf *sbuf_new(struct sbuf *s, char *buf, int length, int flags);
int	sbuf_printf(struct sbuf *s, const char *fmt, ...) __printflike(2, 3);
int	sbuf_vprintf(struct sbuf *s, const char *fmt, va_list ap)
	    __printflike(2, 0);
int	sbuf_cat(struct sbuf *s, const char *str);
int	sbuf_finish(struct sbuf *s);
char	*sbuf_data(struct sbuf *s);
void	sbuf_delete(struct sbuf *s);

/* ---- malloc(9) ------------------------------------------------------------ */

struct malloc_type {
	const char	*ks_shortdesc;
};
#define MALLOC_DEFINE(type, shortdesc, longdesc)			\
	struct malloc_type type[1] = { { shortdesc } }
#define MALLOC_DECLARE(type)	extern struct malloc_type type[1]

#define M_NOWAIT	0x0001
#define M_WAITOK	0x0002
#define M_ZERO		0x0100

void	*mps_compat_malloc(size_t size, struct malloc_type *type, int flags);
void	 mps_compat_free(void *addr, struct malloc_type *type);
#ifndef MPS_COMPAT_NO_LIBC_REMAP
#define malloc(size, type, flags)	mps_compat_malloc((size), (type), (flags))
#define free(addr, type)		mps_compat_free((addr), (type))
#endif

/* ---- time: hz = 1000, ticks, sbintime, timeval --------------------------- */

#define hz		1000
#define ticks		((int)mps_compat_ticks())
#define SBT_1S		((sbintime_t)1 << 32)
#define SBT_1MS		(SBT_1S / 1000)
#define SBT_1US		(SBT_1S / 1000000)
#define C_PREL(x)	0
#define C_DIRECT_EXEC	0

/* XNU: own name so that it never clashes with a platform struct timeval */
#define timeval		mps_timeval
struct mps_timeval {
	long	tv_sec;
	long	tv_usec;
};
#define timevalclear(tvp)	((tvp)->tv_sec = (tvp)->tv_usec = 0)
void	timevalsub(struct timeval *t1, const struct timeval *t2);
void	getmicrouptime(struct timeval *tv);
void	getmicrotime(struct timeval *tv);
sbintime_t sbinuptime(void);
#define sbintime_getsec(sbt)	((int)((sbt) >> 32))
int	ratecheck(struct timeval *lasttime, const struct timeval *mininterval);
uint64_t mps_compat_ticks(void);

/* curthread->td_no_sleeping: the core never runs in interrupt filters */
struct thread {
	int	td_no_sleeping;
};
extern struct thread mps_compat_thread0;
#define curthread	(&mps_compat_thread0)


/* ---- mutex(9), sleep(9), DELAY(9) ---------------------------------------- */

struct callout;

struct mtx {
	void			*mtx_os;	/* mps_os_lock_alloc() */
	void			*mtx_owner;	/* mps_os_curthread() or NULL */
	const char		*mtx_name;
	TAILQ_HEAD(, callout)	 mtx_callouts;	/* armed callouts, by deadline */
	uint64_t		 mtx_armed_us;	/* deadline passed to mps_os_callout_arm */
	struct mtx		*mtx_next;	/* all mutexes, for DELAY() */
};

#define MTX_DEF		0x0000
#define MA_OWNED	0x01
#define MA_NOTOWNED	0x02
#define PRIBIO		0

void	mtx_init(struct mtx *m, const char *name, const char *type, int opts);
void	mtx_destroy(struct mtx *m);
void	mtx_lock(struct mtx *m);
void	mtx_unlock(struct mtx *m);
#define mtx_owned(m)	((m)->mtx_owner == mps_os_curthread())
#define mtx_assert(m, what) do {					\
	if ((what) == MA_OWNED)						\
		KASSERT(mtx_owned(m), ("mutex %s not owned", (m)->mtx_name)); \
	else								\
		KASSERT(!mtx_owned(m), ("mutex %s owned", (m)->mtx_name)); \
} while (0)

/*
 * msleep() drops `mtx' while waiting (mps_os_sleep).  timo is in ticks
 * (1 ms), 0 = no timeout.  Returns 0 when woken, EWOULDBLOCK on timeout.
 */
int	msleep(void *chan, struct mtx *mtx, int pri, const char *wmesg, int timo);
void	wakeup(void *chan);
void	pause(const char *wmesg, int timo);
/*
 * XNU: DELAY() of 1 ms or more with a mutex held by the calling thread drops
 * that mutex while waiting (M4 rule: no sleeping with mps_mtx held).
 */
void	DELAY(int us);

/* ---- callout(9): a sorted list per mutex, run by mps_compat_callout_run -- */

struct callout {
	TAILQ_ENTRY(callout)	 c_link;
	struct mtx		*c_mtx;
	uint64_t		 c_time_us;	/* deadline (mps_os_uptime_us) */
	void			(*c_func)(void *);
	void			*c_arg;
	int			 c_flags;
#define CALLOUT_PENDING		0x0001
};

void	callout_init_mtx(struct callout *c, struct mtx *mtx, int flags);
int	callout_reset(struct callout *c, int to_ticks, void (*func)(void *),
	    void *arg);
int	callout_reset_sbt(struct callout *c, sbintime_t sbt, sbintime_t pr,
	    void (*func)(void *), void *arg, int flags);
int	callout_stop(struct callout *c);
int	callout_drain(struct callout *c);
#define callout_pending(c)	(((c)->c_flags & CALLOUT_PENDING) != 0)
#define callout_active(c)	callout_pending(c)

/* ---- taskqueue(9): FIFO, tasks run without any mutex --------------------- */

typedef void task_fn_t(void *context, int pending);

struct task {
	TAILQ_ENTRY(task)	 ta_link;
	u_short			 ta_pending;
	u_short			 ta_priority;
	task_fn_t		*ta_func;
	void			*ta_context;
};

#define TASK_INIT(task, priority, func, context) do {			\
	(task)->ta_pending = 0;						\
	(task)->ta_priority = (priority);				\
	(task)->ta_func = (func);					\
	(task)->ta_context = (context);					\
} while (0)

struct taskqueue {
	TAILQ_HEAD(, task)	 tq_queue;
	void			*tq_lock;	/* mps_os_lock_alloc() */
	struct task		*tq_running;
	int			 tq_active;	/* a thread runs the queue */
	const char		*tq_name;
	void			*tq_os;		/* platform: thread call */
	int			 tq_flags;
#define TQ_FLAGS_STOPPED	0x0001
};

typedef void taskqueue_enqueue_fn(void *context);
#define taskqueue_thread_enqueue	((taskqueue_enqueue_fn *)NULL)

struct taskqueue *taskqueue_create(const char *name, int mflags,
	    taskqueue_enqueue_fn *enqueue, void *context);
int	taskqueue_start_threads(struct taskqueue **tqp, int count, int pri,
	    const char *name, ...) __printflike(4, 5);
int	taskqueue_enqueue(struct taskqueue *tq, struct task *task);
void	taskqueue_drain(struct taskqueue *tq, struct task *task);
void	taskqueue_free(struct taskqueue *tq);

/* ---- config_intrhook(9): run by mps_compat_intrhook_run() ---------------- */

struct intr_config_hook {
	TAILQ_ENTRY(intr_config_hook) ich_links;
	void	(*ich_func)(void *arg);
	void	*ich_arg;
};
int	config_intrhook_establish(struct intr_config_hook *hook);
void	config_intrhook_disestablish(struct intr_config_hook *hook);

/* ---- bus_space(9) ---------------------------------------------------------- */

#define bus_space_read_4(t, h, o)	mps_os_bus_read_4((t), (h), (o))
#define bus_space_write_4(t, h, o, v)	mps_os_bus_write_4((t), (h), (o), (v))

/* ---- bus_dma(9) ------------------------------------------------------------ */

#define BUS_SPACE_MAXADDR_32BIT		0xFFFFFFFFULL
#define BUS_SPACE_MAXADDR		0xFFFFFFFFFFFFFFFFULL
#define BUS_SPACE_MAXSIZE_24BIT		0xFFFFFFULL
#define BUS_SPACE_MAXSIZE_32BIT		0xFFFFFFFFULL
#define BUS_SPACE_MAXSIZE		0xFFFFFFFFFFFFFFFFULL
#define BUS_SPACE_UNRESTRICTED		(~0)

#define BUS_DMA_WAITOK		0x00
#define BUS_DMA_NOWAIT		0x01
#define BUS_DMA_ALLOCNOW	0x02
#define BUS_DMA_COHERENT	0x04
#define BUS_DMA_ZERO		0x08

#define BUS_DMASYNC_PREREAD	1
#define BUS_DMASYNC_POSTREAD	2
#define BUS_DMASYNC_PREWRITE	4
#define BUS_DMASYNC_POSTWRITE	8

typedef struct bus_dma_segment {
	bus_addr_t	ds_addr;
	bus_size_t	ds_len;
} bus_dma_segment_t;

typedef void bus_dmamap_callback_t(void *, bus_dma_segment_t *, int, int);
typedef void bus_dmamap_callback2_t(void *, bus_dma_segment_t *, int,
    bus_size_t, int);

#define busdma_lock_mutex	((bus_dma_lock_t *)NULL)

typedef struct bus_dma_tag bus_dma_template_t;

enum bus_dma_param_id {
	BD_PARAM_PARENT, BD_PARAM_ALIGNMENT, BD_PARAM_BOUNDARY,
	BD_PARAM_LOWADDR, BD_PARAM_HIGHADDR, BD_PARAM_MAXSIZE,
	BD_PARAM_NSEGMENTS, BD_PARAM_MAXSEGSIZE, BD_PARAM_FLAGS,
	BD_PARAM_LOCKFUNC, BD_PARAM_LOCKFUNCARG, BD_PARAM_NAME
};
typedef struct {
	enum bus_dma_param_id	id;
	uint64_t		num;
} bus_dma_param_t;

#define BD_ALIGNMENT(val)	{ BD_PARAM_ALIGNMENT, (uint64_t)(val) }
#define BD_BOUNDARY(val)	{ BD_PARAM_BOUNDARY, (uint64_t)(val) }
#define BD_LOWADDR(val)		{ BD_PARAM_LOWADDR, (uint64_t)(val) }
#define BD_HIGHADDR(val)	{ BD_PARAM_HIGHADDR, (uint64_t)(val) }
#define BD_MAXSIZE(val)		{ BD_PARAM_MAXSIZE, (uint64_t)(val) }
#define BD_NSEGMENTS(val)	{ BD_PARAM_NSEGMENTS, (uint64_t)(val) }
#define BD_MAXSEGSIZE(val)	{ BD_PARAM_MAXSEGSIZE, (uint64_t)(val) }
#define BD_FLAGS(val)		{ BD_PARAM_FLAGS, (uint64_t)(val) }
#define BD_LOCKFUNC(val)	{ BD_PARAM_LOCKFUNC, 0 }
#define BD_LOCKFUNCARG(val)	{ BD_PARAM_LOCKFUNCARG, 0 }

#define BUS_DMA_TEMPLATE_FILL(t, ...) do {				\
	bus_dma_param_t pm[] = { __VA_ARGS__ };				\
	bus_dma_template_fill(t, pm, howmany(sizeof(pm), sizeof(pm[0]))); \
} while (0)

void	bus_dma_template_init(bus_dma_template_t *t, bus_dma_tag_t parent);
void	bus_dma_template_fill(bus_dma_template_t *t, bus_dma_param_t *kv,
	    u_int count);
int	bus_dma_template_tag(bus_dma_template_t *t, bus_dma_tag_t *dmat);
void	bus_dma_template_clone(bus_dma_template_t *t, bus_dma_tag_t dmat);
int	bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
	    bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
	    void *filter, void *filterarg, bus_size_t maxsize, int nsegments,
	    bus_size_t maxsegsz, int flags, bus_dma_lock_t *lockfunc,
	    void *lockfuncarg, bus_dma_tag_t *dmat);
int	bus_dma_tag_destroy(bus_dma_tag_t dmat);

int	bus_dmamem_alloc(bus_dma_tag_t dmat, void **vaddr, int flags,
	    bus_dmamap_t *mapp);
void	bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map);
int	bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp);
int	bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map);
/*
 * Loads a bus_dmamem_alloc() buffer (any number of segments, as the platform
 * maps it within the tag's constraints).  Arbitrary kernel virtual addresses
 * are not supported (XNU: the core never maps such buffers): EINVAL.
 */
int	bus_dmamap_load(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf,
	    bus_size_t buflen, bus_dmamap_callback_t *callback,
	    void *callback_arg, int flags);
void	bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map);
/* XNU: lend bus_dmamem_alloc() memory to a map until its next unload */
void	bus_dmamap_xnu_lend(bus_dmamap_t map, bus_dmamap_t mem);
/* XNU: PCIe DMA on Apple silicon (DART) is coherent; nothing to do. */
static __inline void
bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map, int op)
{
	(void)dmat;
	(void)map;
	(void)op;
}

/* ---- driver-wide entry points for the platform glue and the test -------- */

/* Run every callout of `m' whose deadline has passed (takes `m'). */
void	mps_compat_callout_run(struct mtx *m);
/* Run every queued task of `tq', in order, without any mutex. */
void	mps_compat_taskqueue_run(struct taskqueue *tq);
/* Run (and remove) the established config_intrhooks. */
void	mps_compat_intrhook_run(void);
/* Live mps_compat_malloc() allocations (leak check). */
long	mps_compat_malloc_count(void);
/* Live DMA allocations made through bus_dmamem_alloc(). */
long	mps_compat_dmamem_count(void);

/* ---- platform hooks (C linkage) ------------------------------------------ */

/* memory */
void	*mps_os_malloc(size_t size, int canwait);
void	 mps_os_free(void *ptr, size_t size);

/* locks: a blocking mutex; mps_os_sleep() is entered with `lock' held */
void	*mps_os_lock_alloc(const char *name);
void	 mps_os_lock_free(void *lock);
void	 mps_os_lock(void *lock);
void	 mps_os_unlock(void *lock);
void	*mps_os_curthread(void);
/* drops `lock' while waiting on `chan'; 0 when woken, EWOULDBLOCK on timeout */
int	 mps_os_sleep(void *lock, void *chan, uint64_t timo_us);
void	 mps_os_wakeup(void *chan);
/* waits without any core lock held by the caller (DELAY, pause) */
void	 mps_os_delay(uint64_t us);

/* clocks */
uint64_t mps_os_uptime_us(void);			/* monotonic */
void	 mps_os_walltime(uint64_t *sec, uint32_t *usec);	/* calendar */

/* registers */
uint32_t mps_os_bus_read_4(bus_space_tag_t t, bus_space_handle_t h,
	    bus_size_t off);
void	 mps_os_bus_write_4(bus_space_tag_t t, bus_space_handle_t h,
	    bus_size_t off, uint32_t val);

/*
 * DMA memory within `tag' constraints (dt_maxsize bytes, dt_alignment,
 * addresses <= dt_lowaddr, at most dt_nsegments segments of dt_maxsegsz).
 * load: the bus addresses of the whole buffer, in order (CPU-contiguous).
 */
int	 mps_os_dmamem_alloc(bus_dma_tag_t tag, void **vaddr, void **cookie);
void	 mps_os_dmamem_free(bus_dma_tag_t tag, void *vaddr, void *cookie);
int	 mps_os_dmamem_load(bus_dma_tag_t tag, void *cookie,
	    bus_dma_segment_t *segs, int maxsegs, int *nsegs);
void	 mps_os_dmamem_unload(bus_dma_tag_t tag, void *cookie);

/* callouts: run mps_compat_callout_run(m) at deadline_us (0: disarm) */
void	 mps_os_callout_arm(struct mtx *m, uint64_t deadline_us);

/* taskqueues: run mps_compat_taskqueue_run(tq) on a thread */
int	 mps_os_taskqueue_start(struct taskqueue *tq);
void	 mps_os_taskqueue_kick(struct taskqueue *tq);
void	 mps_os_taskqueue_wait(struct taskqueue *tq);	/* drain: let it progress */
void	 mps_os_taskqueue_stop(struct taskqueue *tq);	/* no more runs; waits */

/* logging: one line, no newline; cls = mps_dprint classes (0: always) */
void	 mps_os_log(device_t dev, uint32_t cls, const char *line);
/* KASSERT failure / panic: host aborts, kext logs a fault */
void	 mps_os_assert_fail(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* !_MPS_COMPAT_H_ */
