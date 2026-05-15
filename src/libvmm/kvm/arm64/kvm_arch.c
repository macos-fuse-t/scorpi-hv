/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/ioctl.h>

#include <errno.h>
#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../internal.h"

const char *
kvm_arch_backend_name(void)
{
	return ("kvm/arm64");
}

int
kvm_arch_vcpu_init(struct vcpu *vcpu)
{
	struct kvm_vcpu_init init;

	if (vcpu == NULL || vcpu->ctx == NULL || vcpu->ctx->vm_fd < 0 ||
	    vcpu->fd < 0) {
		errno = EINVAL;
		return (-1);
	}

	memset(&init, 0, sizeof(init));
	if (ioctl(vcpu->ctx->vm_fd, KVM_ARM_PREFERRED_TARGET, &init) < 0)
		return (-1);
	if (ioctl(vcpu->fd, KVM_ARM_VCPU_INIT, &init) < 0)
		return (-1);
	return (0);
}

static uint64_t
kvm_arm64_core_reg_id(size_t reg_offset)
{
	return (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
	    reg_offset);
}

static int
kvm_arm64_reg_id(int reg, uint64_t *id)
{
	if (reg >= VM_REG_GUEST_X0 && reg <= VM_REG_GUEST_X29) {
		*id = kvm_arm64_core_reg_id(
		    KVM_REG_ARM_CORE_REG(regs.regs[reg - VM_REG_GUEST_X0]));
		return (0);
	}

	switch (reg) {
	case VM_REG_GUEST_LR:
		*id = kvm_arm64_core_reg_id(
		    KVM_REG_ARM_CORE_REG(regs.regs[30]));
		return (0);
	case VM_REG_GUEST_SP:
		*id = kvm_arm64_core_reg_id(KVM_REG_ARM_CORE_REG(regs.sp));
		return (0);
	case VM_REG_GUEST_PC:
		*id = kvm_arm64_core_reg_id(KVM_REG_ARM_CORE_REG(regs.pc));
		return (0);
	case VM_REG_GUEST_CPSR:
		*id = kvm_arm64_core_reg_id(KVM_REG_ARM_CORE_REG(regs.pstate));
		return (0);
	default:
		errno = ENOTSUP;
		return (-1);
	}
}

int
kvm_arch_set_register(struct vcpu *vcpu, int reg, uint64_t val)
{
	struct kvm_one_reg one_reg;
	uint64_t id;

	if (kvm_arm64_reg_id(reg, &id) != 0)
		return (-1);

	one_reg.id = id;
	one_reg.addr = (uintptr_t)&val;
	if (ioctl(vcpu->fd, KVM_SET_ONE_REG, &one_reg) < 0)
		return (-1);
	return (0);
}

int
kvm_arch_get_register(struct vcpu *vcpu, int reg, uint64_t *retval)
{
	struct kvm_one_reg one_reg;
	uint64_t id;

	if (retval == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (kvm_arm64_reg_id(reg, &id) != 0)
		return (-1);

	one_reg.id = id;
	one_reg.addr = (uintptr_t)retval;
	if (ioctl(vcpu->fd, KVM_GET_ONE_REG, &one_reg) < 0)
		return (-1);
	return (0);
}
