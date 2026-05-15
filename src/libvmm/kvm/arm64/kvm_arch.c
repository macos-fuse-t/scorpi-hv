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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"

#include "../internal.h"

#define	GIC_SPI_BASE		32
#define	GIC_MIN_IRQS		64
#define	GIC_MAX_IRQS		1024
#define	GIC_IRQ_GRANULE		32
#define	GIC_REDIST_STRIDE	(2 * 64 * 1024)

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

static int
kvm_device_set_attr(int fd, uint32_t group, uint64_t attr_id, void *addr)
{
	struct kvm_device_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.group = group;
	attr.attr = attr_id;
	attr.addr = (uintptr_t)addr;
	return (ioctl(fd, KVM_SET_DEVICE_ATTR, &attr));
}

static uint32_t
kvm_vgic_irq_count(uint32_t spi_intid_base, uint32_t spi_intid_count)
{
	uint32_t irq_count;

	if (spi_intid_count == 0)
		return (GIC_MIN_IRQS);

	irq_count = spi_intid_base + spi_intid_count;
	if (irq_count < GIC_MIN_IRQS)
		irq_count = GIC_MIN_IRQS;
	if (irq_count > GIC_MAX_IRQS)
		irq_count = GIC_MAX_IRQS;

	return ((irq_count + GIC_IRQ_GRANULE - 1) & ~(GIC_IRQ_GRANULE - 1));
}

static unsigned int
kvm_created_vcpu_count(struct vmctx *ctx)
{
	unsigned int count;

	count = 0;
	for (int i = 0; i < MAX_VCPUS; i++) {
		if (ctx->vcpus[i] != NULL && ctx->vcpus[i]->fd >= 0)
			count++;
	}
	return (count);
}

int
kvm_arch_attach_vgic(struct vmctx *ctx, uint64_t dist_start,
    size_t dist_size, uint64_t redist_start, size_t redist_size,
    uint64_t mmio_base __unused, uint32_t spi_intid_base,
    uint32_t spi_intid_count)
{
	struct kvm_create_device dev;
	uint32_t irq_count;
	unsigned int vcpu_count;
	int saved_errno;

	if (ctx == NULL || ctx->vm_fd < 0) {
		errno = EINVAL;
		return (-1);
	}
	if (ctx->vgic_fd >= 0)
		return (0);

	vcpu_count = kvm_created_vcpu_count(ctx);
	if (vcpu_count == 0) {
		errno = ENODEV;
		return (-1);
	}
	if ((dist_start & (64 * 1024 - 1)) != 0 ||
	    (redist_start & (64 * 1024 - 1)) != 0 ||
	    dist_size < 64 * 1024 ||
	    redist_size < vcpu_count * GIC_REDIST_STRIDE) {
		errno = EINVAL;
		return (-1);
	}

	memset(&dev, 0, sizeof(dev));
	dev.type = KVM_DEV_TYPE_ARM_VGIC_V3;
	dev.fd = -1;
	if (ioctl(ctx->vm_fd, KVM_CREATE_DEVICE, &dev) < 0) {
		if (errno == ENODEV || errno == EINVAL)
			EPRINTLN("KVM VGICv3 is not supported by this host");
		else
			EPRINTLN("KVM_CREATE_DEVICE(VGICv3) failed: %s",
			    strerror(errno));
		return (-1);
	}

	irq_count = kvm_vgic_irq_count(spi_intid_base, spi_intid_count);
	if (kvm_device_set_attr(dev.fd, KVM_DEV_ARM_VGIC_GRP_NR_IRQS, 0,
	    &irq_count) < 0)
		goto fail;
	if (kvm_device_set_attr(dev.fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
	    KVM_VGIC_V3_ADDR_TYPE_DIST, &dist_start) < 0)
		goto fail;
	if (kvm_device_set_attr(dev.fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
	    KVM_VGIC_V3_ADDR_TYPE_REDIST, &redist_start) < 0)
		goto fail;
	if (kvm_device_set_attr(dev.fd, KVM_DEV_ARM_VGIC_GRP_CTRL,
	    KVM_DEV_ARM_VGIC_CTRL_INIT, NULL) < 0)
		goto fail;

	ctx->vgic_fd = dev.fd;
	return (0);

fail:
	saved_errno = errno;
	close(dev.fd);
	errno = saved_errno;
	return (-1);
}
