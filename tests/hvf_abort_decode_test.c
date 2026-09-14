/* SPDX-License-Identifier: BSD-2-Clause */
#define hv_vcpu_get_reg test_get_reg
#define hv_vcpu_get_sys_reg test_get_sys_reg
#include "../src/libvmm/hvf/vmmapi.c"

int raw_stdio;
static unsigned int register_reads;

hv_return_t
test_get_reg(hv_vcpu_t cpu, hv_reg_t reg, uint64_t *value)
{
	(void)cpu;
	assert(reg == HV_REG_CPSR);
	register_reads++;
	*value = PSR_M_EL1h;
	return (HV_SUCCESS);
}

hv_return_t
test_get_sys_reg(hv_vcpu_t cpu, hv_sys_reg_t reg, uint64_t *value)
{
	(void)cpu; (void)reg;
	register_reads++;
	*value = 0;
	return (HV_SUCCESS);
}

int
main(void)
{
	struct vm_exit exit, before;
	/* Invalid syndrome must be rejected before reading CPU state or operands. */
	uint32_t invalid[] = {0, ISS_DATA_WnR,
	    ISS_DATA_SRT_MASK | ISS_DATA_SAS_MASK,
	    ISS_DATA_ISV | ISS_DATA_CM, ISS_DATA_ISV | ISS_DATA_S1PTW};
	for (unsigned int i = 0; i < nitems(invalid); i++) {
		memset(&exit, 0x5a, sizeof(exit));
		before = exit;
		assert(arm64_gen_inst_emul_data(NULL, invalid[i], &exit) == ENOTSUP);
		assert(memcmp(&exit, &before, sizeof(exit)) == 0);
	}
	assert(register_reads == 0);

	struct vmctx ctx = {0};
	hv_vcpu_exit_t hvexit = {0};
	struct vcpu cpu = {0};
	cpu.ctx = &ctx;
	cpu.vcpu_exit = &hvexit;
	hvexit.exception.physical_address = 0xa8000000;
	assert(arm64_gen_inst_emul_data(&cpu,
	    ISS_DATA_ISV | (3U << ISS_DATA_SAS_SHIFT) |
	    (7U << ISS_DATA_SRT_SHIFT) | ISS_DATA_WnR, &exit) == 0);
	assert(exit.u.inst_emul.gpa == 0xa8000000);
	assert(exit.u.inst_emul.vie.reg == 7);
	assert(exit.u.inst_emul.vie.access_size == 8);
	assert(exit.u.inst_emul.vie.dir == VM_DIR_WRITE);
	puts("HVF abort decode validation passed");
	return (0);
}
