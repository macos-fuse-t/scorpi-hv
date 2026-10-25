/* SPDX-License-Identifier: BSD-2-Clause */
#include "../src/libvmm/hvf/vmmapi.c"

int raw_stdio;

int
main(void)
{
	struct vmctx ctx = {0};
	struct vcpu cpu = {.ctx = &ctx};
	struct vcpu secondary = {.ctx = &ctx};

	/* A timer kick after consuming reset must not reset a VP again. Each
	 * VP consumes its own request; CPU 0 cannot consume another VP's reset.
	 */
	atomic_store(&cpu.pending_suspend, VM_SUSPEND_RESET);
	atomic_store(&secondary.pending_suspend, VM_SUSPEND_RESET);
	assert(vcpu_take_suspend(&cpu) == VM_SUSPEND_RESET);
	assert(vcpu_take_suspend(&cpu) == VM_SUSPEND_NONE);
	assert(vcpu_take_suspend(&secondary) == VM_SUSPEND_RESET);
	assert(vcpu_take_suspend(&secondary) == VM_SUSPEND_NONE);
	atomic_store(&cpu.pending_suspend, VM_SUSPEND_POWEROFF);
	assert(vcpu_take_suspend(&cpu) == VM_SUSPEND_POWEROFF);
	assert(vcpu_take_suspend(&cpu) == VM_SUSPEND_NONE);

	puts("HVF suspend requests are consumed independently without stale resets");
	return (0);
}
