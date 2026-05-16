/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#include <vmm.h>
#include <vmmapi.h>
#include <vmm_instruction_emul.h>

int
vmm_emulate_instruction(struct vcpu *vcpu __unused, uint64_t gpa __unused,
    struct vie *vie __unused, struct vm_guest_paging *paging __unused,
    mem_region_read_t memread __unused, mem_region_write_t memwrite __unused,
    void *memarg __unused)
{
	return (EOPNOTSUPP);
}

int
vie_update_register(struct vcpu *vcpu __unused, enum vm_reg_name reg __unused,
    uint64_t val __unused, int size __unused)
{
	return (EOPNOTSUPP);
}

int
vie_alignment_check(int cpl __unused, int operand_size __unused,
    uint64_t cr0 __unused, uint64_t rflags __unused, uint64_t gla __unused)
{
	return (0);
}

int
vie_canonical_check(enum vm_cpu_mode cpu_mode __unused, uint64_t gla __unused)
{
	return (0);
}

uint64_t
vie_size2mask(int size)
{
	if (size >= 8)
		return (~0ULL);
	return ((1ULL << (size * 8)) - 1);
}

int
vie_calculate_gla(enum vm_cpu_mode cpu_mode __unused,
    enum vm_reg_name seg __unused, struct seg_desc *desc __unused,
    uint64_t off __unused, int length __unused, int addrsize __unused,
    int prot __unused, uint64_t *gla __unused)
{
	return (EOPNOTSUPP);
}

void
vie_restart(struct vie *vie)
{
	vie->num_processed = 0;
}

void
vie_init(struct vie *vie, const char *inst_bytes, int inst_length)
{
	memset(vie, 0, sizeof(*vie));
	if (inst_length > VIE_INST_SIZE)
		inst_length = VIE_INST_SIZE;
	memcpy(vie->inst, inst_bytes, inst_length);
	vie->num_valid = inst_length;
}

int
vmm_decode_instruction(enum vm_cpu_mode cpu_mode __unused, int csd __unused,
    struct vie *vie __unused)
{
	return (EOPNOTSUPP);
}
