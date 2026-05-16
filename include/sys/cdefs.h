/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#pragma once

#if defined(__has_include_next)
#if __has_include_next(<sys/cdefs.h>)
#include_next <sys/cdefs.h>
#endif
#elif defined(__GNUC__)
#include_next <sys/cdefs.h>
#endif

#if !defined(__unused)
#define	__unused	__attribute__((__unused__))
#endif

#if !defined(__printflike)
#define	__printflike(fmtarg, firstvararg) \
	__attribute__((__format__(__printf__, fmtarg, firstvararg)))
#endif

#if !defined(__offsetof)
#define	__offsetof(type, field)	__builtin_offsetof(type, field)
#endif

#undef __CONCAT
#define	__CONCAT1(x, y)	x ## y
#define	__CONCAT(x, y)	__CONCAT1(x, y)

#if !defined(__weak)
#define	__weak	__attribute__((__weak__))
#endif

#if !defined(__FBSDID)
#define	__FBSDID(s)	struct __hack
#endif

#if !defined(__DECONST)
#define	__DECONST(type, var)	((type)(__UINTPTR_TYPE__)(const void *)(var))
#endif
