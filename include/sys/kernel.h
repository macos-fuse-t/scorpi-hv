/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#pragma once

#include <sys/param.h>

#ifndef	MIN
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#endif

#ifndef	MAX
#define	MAX(a, b)	((a) > (b) ? (a) : (b))
#endif

#ifndef	howmany
#define	howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif

#ifndef	powerof2
#define	powerof2(x)	((((x) - 1) & (x)) == 0)
#endif

#ifndef	SYSRES_MEM
#define	SYSRES_MEM(base, size)
#endif
