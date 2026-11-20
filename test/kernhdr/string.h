/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 */
/* Stand-in for Kernel.framework <string.h>: freestanding compile check only. */
#ifndef _KERNHDR_STRING_H_
#define _KERNHDR_STRING_H_
#include <stddef.h>
void	*memcpy(void *, const void *, size_t);
void	*memmove(void *, const void *, size_t);
void	*memset(void *, int, size_t);
int	 memcmp(const void *, const void *, size_t);
size_t	 strlen(const char *);
int	 strcmp(const char *, const char *);
int	 strncmp(const char *, const char *, size_t);
char	*strchr(const char *, int);
#endif
