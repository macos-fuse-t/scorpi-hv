/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#pragma once

#include <errno.h>
#include <err.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>

#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define	__BEGIN_DECLS	extern "C" {
#define	__END_DECLS	}
#else
#define	__BEGIN_DECLS
#define	__END_DECLS
#endif
#endif

#ifndef __FBSDID
#define	__FBSDID(id)
#endif

#ifndef __unused
#define	__unused	__attribute__((__unused__))
#endif

#ifndef __packed
#define	__packed	__attribute__((__packed__))
#endif

#ifndef __aligned
#define	__aligned(x)	__attribute__((__aligned__(x)))
#endif

#ifndef __printflike
#if defined(__GNUC__) || defined(__clang__)
#define	__printflike(fmtarg, firstvararg) \
	__attribute__((__format__(__printf__, fmtarg, firstvararg)))
#else
#define	__printflike(fmtarg, firstvararg)
#endif
#endif

#ifndef __DECONST
#define	__DECONST(type, var)	((type)(uintptr_t)(const void *)(var))
#endif

#ifndef __predict_false
#define	__predict_false(exp)	__builtin_expect((exp), 0)
#endif

#ifndef __predict_true
#define	__predict_true(exp)	__builtin_expect((exp), 1)
#endif

#ifndef __containerof
#define	__containerof(ptr, type, member) \
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif

#ifndef MAXCOMLEN
#define	MAXCOMLEN	16
#endif

#ifndef NBBY
#define	NBBY	8
#endif

#ifndef howmany
#define	howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif

#ifndef MAXPHYS
#define	MAXPHYS	(128 * 1024)
#endif

#ifndef KB
#define	KB	(1024UL)
#endif

#ifndef MB
#define	MB	(1024UL * KB)
#endif

#ifndef GB
#define	GB	(1024UL * MB)
#endif

#ifndef OFF_MAX
#define	OFF_MAX	((off_t)INT64_MAX)
#endif

#ifndef PSHMNAMLEN
#ifdef NAME_MAX
#define	PSHMNAMLEN	NAME_MAX
#else
#define	PSHMNAMLEN	255
#endif
#endif

#if defined(__linux__)
#ifndef _UUID_STRING_T
#define	_UUID_STRING_T
typedef char uuid_string_t[37];
#endif

static inline void
warnc(int code, const char *fmt, ...)
{
	int saved_errno;
	va_list ap;

	saved_errno = errno;
	errno = code;
	va_start(ap, fmt);
	vwarn(fmt, ap);
	va_end(ap);
	errno = saved_errno;
}
#endif
