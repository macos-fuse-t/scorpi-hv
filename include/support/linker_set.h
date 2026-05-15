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

#if !defined(__used)
# define __used __attribute__((used))
#endif

#if defined(__APPLE__)
/*
 * Put each set entry (pointer) in the "__DATA,__<set>" section.  Mark it as
 * used so LTO doesn't discard it.
 */
#define __MAKE_SET(set, sym) \
__attribute__((no_sanitize("address"), used, section("__DATA,__" #set))) \
    static void const *__set_##set##_sym_##sym = &(sym)

/* For convenience: */
#define DATA_SET(set, sym)  __MAKE_SET(set, sym)

/*
 * Provide Mach-O boundary symbols:
 *   section$start$__DATA$__<set>
 *   section$end$__DATA$__<set>
 */
#define SET_DECLARE(set, T) \
    extern T *__start_##set[] __asm("section$start$__DATA$__" #set); \
    extern T *__stop_##set[]  __asm("section$end$__DATA$__" #set)
#else
#define __MAKE_SET(set, sym) \
__attribute__((used, section(#set))) \
    static void const *__set_##set##_sym_##sym = &(sym)

#define DATA_SET(set, sym)  __MAKE_SET(set, sym)

#define SET_DECLARE(set, T) \
    extern T *__start_##set[] __attribute__((weak)); \
    extern T *__stop_##set[] __attribute__((weak))
#endif

/* Common iteration macros: */
#define SET_BEGIN(set) (__start_##set)
#define SET_LIMIT(set) (__stop_##set)
#define SET_COUNT(set) (SET_LIMIT(set) - SET_BEGIN(set))
#define SET_FOREACH(pvar, set) \
    for ((pvar) = (SET_BEGIN(set) == NULL || SET_LIMIT(set) == NULL) ? \
	    NULL : SET_BEGIN(set); \
	(pvar) != NULL && (pvar) < SET_LIMIT(set); (pvar)++)
