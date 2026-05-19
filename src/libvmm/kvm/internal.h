/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <support/cpuset.h>
#include <vmm.h>

enum {
	VM_MEMSEG_LOW,
	VM_MEMSEG_HIGH,
	VM_MEMSEG_COUNT,
};

#define	VM_MAX_MEMORY_SEGMENTS	16
#define	MAX_VCPUS		64

struct kvm_mem_range {
	uint64_t gpa;
	uint64_t prot;
	size_t len;
	void *object;
	int slot;
	bool owned;
	bool active;
};

struct vmctx {
	struct {
		vm_paddr_t base;
		vm_size_t size;
		uintptr_t addr;
	} memsegs[VM_MEMSEG_COUNT];

	int num_mem_ranges;
	struct kvm_mem_range mem_ranges[VM_MAX_MEMORY_SEGMENTS];
	char *name;
	enum vm_suspend_how suspend_reason;
	int memflags;
	uint16_t sockets;
	uint16_t cores;
	uint16_t threads;
	uint16_t maxcpus;

	int kvm_fd;
	int vm_fd;
	int vgic_fd;
#if defined(__aarch64__)
	uint64_t vgic_dist_base;
	uint64_t vgic_msi_base;
	uint32_t vgic_msi_spi_base;
	uint32_t vgic_msi_spi_count;
#endif

	cpuset_t active_cpus;
	cpuset_t suspended_cpus;
	struct vcpu *vcpus[MAX_VCPUS];
};

struct vcpu {
	struct vmctx *ctx;
	int vcpuid;
	int fd;
	void *run;
	size_t run_size;
	bool initialized;
	uint64_t regs[VM_REG_LAST];
	bool reg_set[VM_REG_LAST];
};

const char *kvm_arch_backend_name(void);
int kvm_arch_vcpu_init(struct vcpu *vcpu);
int kvm_arch_set_register(struct vcpu *vcpu, int reg, uint64_t val);
int kvm_arch_get_register(struct vcpu *vcpu, int reg, uint64_t *retval);
int kvm_arch_attach_vgic(struct vmctx *ctx, uint64_t dist_start,
    size_t dist_size, uint64_t redist_start, size_t redist_size,
    uint64_t mmio_base, uint32_t spi_intid_base, uint32_t spi_intid_count);
int kvm_arch_set_irq(struct vmctx *ctx, uint32_t irq, bool level);
int kvm_arch_raise_msi(struct vmctx *ctx, uint64_t addr, uint64_t msg,
    int bus, int slot, int func);
