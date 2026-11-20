/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 */
/* Stand-in for Kernel.framework <libkern/libkern.h>: freestanding compile check only. */
#ifndef _KERNHDR_LIBKERN_H_
#define _KERNHDR_LIBKERN_H_
#include <stdarg.h>
#include <stddef.h>
int	snprintf(char *, size_t, const char *, ...) __attribute__((format(printf, 3, 4)));
int	vsnprintf(char *, size_t, const char *, va_list) __attribute__((format(printf, 3, 0)));
int	printf(const char *, ...) __attribute__((format(printf, 1, 2)));
/* as Kernel.framework's libkern.h */
static inline unsigned int max(unsigned int a, unsigned int b) { return a > b ? a : b; }
static inline unsigned int min(unsigned int a, unsigned int b) { return a < b ? a : b; }
#endif
