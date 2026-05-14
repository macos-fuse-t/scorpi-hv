/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <support/freebsd_compat.h>

#if defined(__GNUC__) || defined(__clang__)
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
#else
#error "Compiler does not support __packed attribute"
#endif

typedef unsigned long    UINT64;
typedef unsigned int    UINT32;
typedef unsigned short    UINT16;
typedef unsigned char     UINT8;

typedef uint64_t    vm_paddr_t;
typedef uintptr_t    vm_offset_t;
typedef uintptr_t    vm_size_t;

#define	PAGE_SHIFT	12
#define	PAGE_SIZE	(1 << PAGE_SHIFT)   // 4096
#define	PAGE_MASK	(PAGE_SIZE - 1)

#define	PAGE_SHIFT_4K	12
#define	PAGE_SIZE_4K	(1 << PAGE_SHIFT_4K)

#define	PAGE_SHIFT_16K	14
#define	PAGE_SIZE_16K	(1 << PAGE_SHIFT_16K)

#define	PAGE_SHIFT_64K	16
#define PAGE_SIZE_64K   (1 << PAGE_SHIFT_64K)
#define	PAGE_MASK_64K   (PAGE_SIZE_64K - 1)

#define	round_page(x)		(((unsigned long)(x) + PAGE_MASK) & ~PAGE_MASK)
#define	trunc_page(x)		((unsigned long)(x) & ~PAGE_MASK)

#define	round_page64k(x)		(((unsigned long)(x) + PAGE_MASK_64K) & ~PAGE_MASK_64K)

#define nitems(x) (sizeof(x) / sizeof((x)[0]))

#define roundup2(x, y)         (((x)+((y)-1))&(~((y)-1)))

#ifndef SIZE_T_MAX
#define	SIZE_T_MAX	SIZE_MAX
#endif

#if defined(__linux__) && !defined(HAVE_REALLOCF)
static inline void *
reallocf(void *ptr, size_t size)
{
	void *newptr;

	newptr = realloc(ptr, size);
	if (newptr == NULL)
		free(ptr);
	return (newptr);
}
#endif

#if defined(__linux__) && !defined(HAVE_FLSL)
static inline int
flsl(long value)
{
	if (value == 0)
		return (0);
	return ((int)(sizeof(long) * 8) - __builtin_clzl(value));
}
#endif
