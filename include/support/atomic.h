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

#include <stdint.h>
#include <stdatomic.h>
#include <strings.h>
#include <sys/types.h>

static inline void atomic_set_long(volatile u_long *ptr, long mask) {
    atomic_fetch_or((volatile _Atomic long *)ptr, mask);
}

static inline void atomic_clear_long(volatile u_long *ptr, long mask) {
    atomic_fetch_and((volatile _Atomic long *)ptr, ~mask);
}

static inline void atomic_store_rel_long(volatile u_long *ptr, long value) {
    atomic_store_explicit((volatile _Atomic long *)ptr, value, memory_order_release);
}

static inline int atomic_cmpset_ptr(long *ptr, long expected, long desired) {
    return atomic_compare_exchange_strong((atomic_long*) ptr, &expected, desired);
}

static inline int32_t atomic_load_32(const int32_t *obj) {
    return __atomic_load_n(obj, __ATOMIC_SEQ_CST);
}

static inline int64_t atomic_load_64(const int64_t *obj) {
    return __atomic_load_n(obj, __ATOMIC_SEQ_CST);
}

static inline int __bitcountl(long value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountl(value);
#else
    // Generic implementation
    int count = 0;
    while (value) {
        count += value & 1;
        value >>= 1;
    }
    return count;
#endif
}

#if !defined(__APPLE__) && !defined(__linux__)
static inline int ffsl(long value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ffsl(value);
#else
    // Generic implementation
    int pos = 1;
    while (value) {
        if (value & 1) {
            return pos;
        }
        value >>= 1;
        pos++;
    }
    return 0;
#endif
}
#endif
