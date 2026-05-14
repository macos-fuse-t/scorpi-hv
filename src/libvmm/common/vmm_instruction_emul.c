/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 */

#include <support/freebsd_compat.h>
#include <sys/errno.h>
#include <sys/types.h>

#include <stdint.h>

#include <vmm.h>
#include <vmmapi.h>
#include <vmm_instruction_emul.h>

int
vmm_emulate_instruction(struct vcpu *vcpu, uint64_t gpa, struct vie *vie,
    struct vm_guest_paging *paging __unused, mem_region_read_t memread,
    mem_region_write_t memwrite, void *memarg)
{
	uint64_t val = 0;
	int error;

	if (vie->dir == VM_DIR_READ) {
		error = memread(vcpu, gpa, &val, vie->access_size, memarg);
		if (error)
			goto out;
		error = vm_set_register(vcpu, vie->reg, val);
	} else {
		error = vm_get_register(vcpu, vie->reg, &val);
		if (error)
			goto out;
		if (vie->access_size < 8)
			val &= (1ul << (vie->access_size * 8)) - 1;
		error = memwrite(vcpu, gpa, val, vie->access_size, memarg);
	}

out:
	return (error);
}

int
vmm_emulate_register(struct vcpu *vcpu, struct vre *vre, reg_read_t regread,
    reg_write_t regwrite, void *regarg)
{
	uint64_t val;
	int error;

	if (vre->dir == VM_DIR_READ) {
		error = regread(vcpu, &val, regarg);
		if (error)
			goto out;
		error = vm_set_register(vcpu, vre->reg, val);
	} else {
		error = vm_get_register(vcpu, vre->reg, &val);
		if (error)
			goto out;
		error = regwrite(vcpu, val, regarg);
	}

out:
	return (error);
}
