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

#include <stdbool.h>

enum {
	VM_MEMSEG_LOW,
	VM_MEMSEG_HIGH,
	VM_MEMSEG_COUNT,
};

#define	VM_MAX_MEMORY_SEGMENTS	16

struct mem_range {
	uint64_t gpa;
	size_t len;
	void *object;
	uint64_t prot;
	bool active;
	bool owned;
	char shm_suffix[64];
};

#define MAX_VCPUS		64

struct vmctx {
    struct {
		vm_paddr_t base;
		vm_size_t size;
        uintptr_t addr;
	} memsegs[VM_MEMSEG_COUNT];

    int             num_mem_ranges;
	struct mem_range  mem_ranges[VM_MAX_MEMORY_SEGMENTS];
	char	*name;
	enum vm_suspend_how suspend_reason;
	bool el2_enabled;

    cpuset_t active_cpus;
    cpuset_t suspended_cpus;
	struct vcpu *vcpus[MAX_VCPUS];
};

int init_apple_vgic(struct vmctx *ctx, uint64_t dist_start, size_t dist_size,
    uint64_t redist_start, size_t redist_size, uint64_t mmio_base, 
    uint32_t spi_intid_base, uint32_t spi_intid_count);
