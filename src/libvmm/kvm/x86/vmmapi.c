/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/uio.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef CPU_AND
#undef CPU_CLR
#undef CPU_ISSET
#undef CPU_SET
#undef CPU_ZERO

#include "vmm.h"

#include <vmmapi.h>

#include "amd64/inout.h"
#include "debug.h"
#include "mem.h"

#define	MB			(1024 * 1024UL)
#define	GB			(1024 * 1024 * 1024UL)
#define	VM_LOWMEM_LIMIT		(3 * GB)
#define	VM_HIGHMEM_BASE		(4 * GB)
#define	VM_MAX_MEMSLOTS		32
#define	KVM_TSS_ADDR		0xfffbd000
#define	KVM_IDENTITY_MAP_ADDR	0xfffbc000
#define	KVM_IOAPIC_PINS		24

enum {
	VM_MEMSEG_LOW,
	VM_MEMSEG_HIGH,
	VM_MEMSEG_COUNT,
};

struct kvm_memslot {
	uint64_t gpa;
	size_t len;
	void *host;
	int slot;
};

struct vmctx {
	int sys_fd;
	int vm_fd;
	int run_mmap_size;
	char *name;
	int memflags;
	enum vm_suspend_how suspend_reason;
	struct {
		vm_paddr_t base;
		vm_size_t size;
		uintptr_t addr;
	} memsegs[VM_MEMSEG_COUNT];
	int nmemslots;
	struct kvm_memslot memslots[VM_MAX_MEMSLOTS];
	cpuset_t active_cpus;
	cpuset_t suspended_cpus;
	struct vcpu *vcpus[CPU_SETSIZE];
};

struct vcpu {
	struct vmctx *ctx;
	int vcpuid;
	int fd;
	struct kvm_run *run;
	size_t run_len;
	pthread_t tid;
};

static int
kvm_ioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret == -1 && errno == EINTR);

	return (ret);
}

static int
kvm_check_extension(struct vmctx *ctx, int cap)
{
	return (kvm_ioctl(ctx->sys_fd, KVM_CHECK_EXTENSION,
	    (void *)(uintptr_t)cap));
}

static int
kvm_set_irq_routing(struct vmctx *ctx)
{
	struct kvm_irq_routing *routing;
	size_t len;
	int ret;

	if (kvm_check_extension(ctx, KVM_CAP_IRQ_ROUTING) <= 0)
		return (0);

	len = sizeof(*routing) + KVM_IOAPIC_PINS * sizeof(routing->entries[0]);
	routing = calloc(1, len);
	if (routing == NULL)
		return (ENOMEM);
	routing->nr = KVM_IOAPIC_PINS;
	for (int i = 0; i < KVM_IOAPIC_PINS; i++) {
		routing->entries[i].gsi = i;
		routing->entries[i].type = KVM_IRQ_ROUTING_IRQCHIP;
		routing->entries[i].u.irqchip.irqchip = KVM_IRQCHIP_IOAPIC;
		routing->entries[i].u.irqchip.pin = i;
	}

	ret = kvm_ioctl(ctx->vm_fd, KVM_SET_GSI_ROUTING, routing);
	free(routing);
	return (ret < 0 ? errno : 0);
}

static int
kvm_set_identity_map(struct vmctx *ctx)
{
	uint64_t addr;

	if (kvm_check_extension(ctx, KVM_CAP_SET_IDENTITY_MAP_ADDR) <= 0)
		return (0);

	addr = KVM_IDENTITY_MAP_ADDR;
	return (kvm_ioctl(ctx->vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &addr) < 0 ?
	    errno : 0);
}

static int
kvm_set_cpuid(struct vcpu *vcpu)
{
	struct kvm_cpuid2 *cpuid;
	int entries, error;
	size_t len;

	entries = kvm_check_extension(vcpu->ctx, KVM_CAP_EXT_CPUID);
	if (entries <= 0)
		entries = 100;

	len = sizeof(*cpuid) + entries * sizeof(cpuid->entries[0]);
	cpuid = calloc(1, len);
	if (cpuid == NULL)
		return (ENOMEM);
	cpuid->nent = entries;

	error = 0;
	if (kvm_ioctl(vcpu->ctx->sys_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0)
		error = errno;
	else if (kvm_ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid) < 0)
		error = errno;

	free(cpuid);
	return (error);
}

int
vm_create(const char *name __unused)
{
	return (0);
}

struct vmctx *
vm_open(const char *name)
{
	return (vm_openf(name, 0));
}

struct vmctx *
vm_openf(const char *name, int flags __unused)
{
	struct vmctx *ctx;
	int api, error;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return (NULL);

	ctx->sys_fd = -1;
	ctx->vm_fd = -1;
	ctx->name = strdup(name != NULL ? name : "scorpi");
	if (ctx->name == NULL)
		goto fail;
	ctx->sys_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (ctx->sys_fd < 0)
		goto fail;

	api = kvm_ioctl(ctx->sys_fd, KVM_GET_API_VERSION, NULL);
	if (api != KVM_API_VERSION) {
		errno = ENXIO;
		goto fail;
	}

	ctx->vm_fd = kvm_ioctl(ctx->sys_fd, KVM_CREATE_VM, NULL);
	if (ctx->vm_fd < 0)
		goto fail;

	ctx->run_mmap_size = kvm_ioctl(ctx->sys_fd, KVM_GET_VCPU_MMAP_SIZE,
	    NULL);
	if (ctx->run_mmap_size <= 0)
		goto fail;

	if (kvm_ioctl(ctx->vm_fd, KVM_SET_TSS_ADDR,
		(void *)(uintptr_t)KVM_TSS_ADDR) < 0) {
		goto fail;
	}

	error = kvm_set_identity_map(ctx);
	if (error != 0) {
		errno = error;
		goto fail;
	}

	if (kvm_ioctl(ctx->vm_fd, KVM_CREATE_IRQCHIP, NULL) < 0)
		goto fail;
	error = kvm_set_irq_routing(ctx);
	if (error != 0) {
		errno = error;
		goto fail;
	}

	ctx->suspend_reason = VM_SUSPEND_NONE;
	CPU_ZERO(&ctx->active_cpus);
	CPU_ZERO(&ctx->suspended_cpus);
	return (ctx);

fail:
	vm_destroy(ctx);
	return (NULL);
}

void
vm_close(struct vmctx *ctx)
{
	if (ctx == NULL)
		return;
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (ctx->vcpus[i] != NULL)
			vm_vcpu_close(ctx->vcpus[i]);
	}
	for (int i = 0; i < ctx->nmemslots; i++)
		free(ctx->memslots[i].host);
	if (ctx->vm_fd >= 0)
		close(ctx->vm_fd);
	if (ctx->sys_fd >= 0)
		close(ctx->sys_fd);
	free(ctx->name);
	free(ctx);
}

void
vm_destroy(struct vmctx *ctx)
{
	vm_close(ctx);
}

int
vm_limit_rights(struct vmctx *ctx __unused)
{
	return (0);
}

struct vcpu *
vm_vcpu_open(struct vmctx *ctx, int vcpuid)
{
	struct vcpu *vcpu;

	if (vcpuid < 0 || vcpuid >= CPU_SETSIZE) {
		errno = EINVAL;
		return (NULL);
	}

	vcpu = calloc(1, sizeof(*vcpu));
	if (vcpu == NULL)
		return (NULL);
	vcpu->ctx = ctx;
	vcpu->vcpuid = vcpuid;
	vcpu->fd = -1;
	ctx->vcpus[vcpuid] = vcpu;
	return (vcpu);
}

int
vm_vcpu_init(struct vcpu *vcpu)
{
	int error;

	if (vcpu->fd >= 0)
		return (0);

	vcpu->fd = kvm_ioctl(vcpu->ctx->vm_fd, KVM_CREATE_VCPU,
	    (void *)(uintptr_t)vcpu->vcpuid);
	if (vcpu->fd < 0)
		return (-1);

	vcpu->run_len = vcpu->ctx->run_mmap_size;
	vcpu->run = mmap(NULL, vcpu->run_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, vcpu->fd, 0);
	if (vcpu->run == MAP_FAILED) {
		close(vcpu->fd);
		vcpu->fd = -1;
		return (-1);
	}
	vcpu->tid = pthread_self();
	error = kvm_set_cpuid(vcpu);
	if (error != 0) {
		vm_vcpu_deinit(vcpu);
		errno = error;
		return (-1);
	}
	return (0);
}

void
vm_vcpu_deinit(struct vcpu *vcpu)
{
	if (vcpu->run != NULL && vcpu->run != MAP_FAILED)
		munmap(vcpu->run, vcpu->run_len);
	vcpu->run = NULL;
	if (vcpu->fd >= 0)
		close(vcpu->fd);
	vcpu->fd = -1;
}

void
vm_vcpu_close(struct vcpu *vcpu)
{
	if (vcpu == NULL)
		return;
	vm_vcpu_deinit(vcpu);
	if (vcpu->ctx != NULL && vcpu->vcpuid >= 0 &&
	    vcpu->vcpuid < CPU_SETSIZE) {
		vcpu->ctx->vcpus[vcpu->vcpuid] = NULL;
	}
	free(vcpu);
}

int
vcpu_id(struct vcpu *vcpu)
{
	return (vcpu->vcpuid);
}

int
vm_parse_memsize(const char *opt, size_t *ret_memsize)
{
	char *endptr;
	size_t val;

	val = strtoull(opt, &endptr, 0);
	if (*opt == '\0')
		return (EINVAL);
	switch (*endptr) {
	case 'g':
	case 'G':
		val *= GB;
		endptr++;
		break;
	case 'm':
	case 'M':
		val *= MB;
		endptr++;
		break;
	case '\0':
		if (val < MB)
			val *= MB;
		break;
	default:
		return (EINVAL);
	}
	if (*endptr != '\0')
		return (EINVAL);
	*ret_memsize = val;
	return (0);
}

uint32_t
vm_get_lowmem_limit(struct vmctx *ctx __unused)
{
	return (VM_LOWMEM_LIMIT);
}

void
vm_set_memflags(struct vmctx *ctx, int flags)
{
	ctx->memflags = flags;
}

int
vm_get_memflags(struct vmctx *ctx)
{
	return (ctx->memflags);
}

static int
vm_add_memslot(struct vmctx *ctx, vm_paddr_t gpa, size_t len, void *host)
{
	struct kvm_userspace_memory_region region;
	struct kvm_memslot *slot;

	if (ctx->nmemslots >= VM_MAX_MEMSLOTS)
		return (E2BIG);

	slot = &ctx->memslots[ctx->nmemslots];
	slot->slot = ctx->nmemslots;
	slot->gpa = gpa;
	slot->len = len;
	slot->host = host;

	memset(&region, 0, sizeof(region));
	region.slot = slot->slot;
	region.guest_phys_addr = gpa;
	region.memory_size = len;
	region.userspace_addr = (uintptr_t)host;

	if (kvm_ioctl(ctx->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
		return (errno);

	ctx->nmemslots++;
	return (0);
}

int
vm_setup_memory_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uint64_t prot __unused, uintptr_t *addr)
{
	void *host;
	int error;

	if ((gpa & PAGE_MASK) != 0 || (len & PAGE_MASK) != 0 || len == 0)
		return (EINVAL);

	if (posix_memalign(&host, PAGE_SIZE, len) != 0)
		return (ENOMEM);
	memset(host, 0, len);

	error = vm_add_memslot(ctx, gpa, len, host);
	if (error != 0) {
		free(host);
		return (error);
	}
	if (addr != NULL)
		*addr = (uintptr_t)host;
	return (0);
}

int
vm_setup_bootrom_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uintptr_t *addr)
{
	return (vm_setup_memory_segment(ctx, gpa, len, PROT_READ | PROT_WRITE,
	    addr));
}

int
vm_setup_memory(struct vmctx *ctx, size_t memsize, enum vm_mmap_style vms)
{
	uintptr_t *addr;
	int error;

	assert(vms == VM_MMAP_NONE || vms == VM_MMAP_ALL);

	ctx->memsegs[VM_MEMSEG_LOW].base = 0;
	ctx->memsegs[VM_MEMSEG_LOW].size = MIN(memsize, VM_LOWMEM_LIMIT);
	ctx->memsegs[VM_MEMSEG_HIGH].base = VM_HIGHMEM_BASE;
	ctx->memsegs[VM_MEMSEG_HIGH].size =
	    memsize > VM_LOWMEM_LIMIT ? memsize - VM_LOWMEM_LIMIT : 0;

	if (ctx->memsegs[VM_MEMSEG_LOW].size > 0) {
		addr = vms == VM_MMAP_ALL ? &ctx->memsegs[VM_MEMSEG_LOW].addr :
		    NULL;
		error = vm_setup_memory_segment(ctx, 0,
		    ctx->memsegs[VM_MEMSEG_LOW].size, PROT_READ | PROT_WRITE,
		    addr);
		if (error != 0)
			return (error);
	}
	if (ctx->memsegs[VM_MEMSEG_HIGH].size > 0) {
		addr = vms == VM_MMAP_ALL ? &ctx->memsegs[VM_MEMSEG_HIGH].addr :
		    NULL;
		error = vm_setup_memory_segment(ctx, VM_HIGHMEM_BASE,
		    ctx->memsegs[VM_MEMSEG_HIGH].size,
		    PROT_READ | PROT_WRITE, addr);
		if (error != 0)
			return (error);
	}
	return (0);
}

void *
vm_map_gpa(struct vmctx *ctx, vm_paddr_t gaddr, size_t len)
{
	struct kvm_memslot *slot;

	for (int i = 0; i < ctx->nmemslots; i++) {
		slot = &ctx->memslots[i];
		if (gaddr >= slot->gpa && len <= slot->len &&
		    gaddr + len <= slot->gpa + slot->len) {
			return ((uint8_t *)slot->host + (gaddr - slot->gpa));
		}
	}
	return (NULL);
}

vm_paddr_t
vm_rev_map_gpa(struct vmctx *ctx, void *addr)
{
	uintptr_t p;

	p = (uintptr_t)addr;
	for (int i = 0; i < ctx->nmemslots; i++) {
		struct kvm_memslot *slot = &ctx->memslots[i];
		uintptr_t start = (uintptr_t)slot->host;
		if (p >= start && p < start + slot->len)
			return (slot->gpa + (p - start));
	}
	return ((vm_paddr_t)-1);
}

int
vm_get_guestmem_from_ctx(struct vmctx *ctx, char **guest_baseaddr,
    size_t *lowmem_size, size_t *highmem_size)
{
	if (guest_baseaddr != NULL)
		*guest_baseaddr = (char *)ctx->memsegs[VM_MEMSEG_LOW].addr;
	if (lowmem_size != NULL)
		*lowmem_size = ctx->memsegs[VM_MEMSEG_LOW].size;
	if (highmem_size != NULL)
		*highmem_size = ctx->memsegs[VM_MEMSEG_HIGH].size;
	return (0);
}

int
vm_get_memseg(struct vmctx *ctx __unused, int ident __unused,
    size_t *lenp __unused, char *name __unused, size_t namesiz __unused)
{
	return (ENOENT);
}

int
vm_mmap_getnext(struct vmctx *ctx __unused, vm_paddr_t *gpa __unused,
    int *segid __unused, vm_offset_t *segoff __unused, size_t *len __unused,
    int *prot __unused, int *flags __unused)
{
	return (ENOENT);
}

int
vm_munmap_memseg(struct vmctx *ctx __unused, vm_paddr_t gpa __unused,
    size_t len __unused)
{
	return (EOPNOTSUPP);
}

const char *
vm_get_name(struct vmctx *ctx)
{
	return (ctx->name);
}

size_t
vm_get_lowmem_size(struct vmctx *ctx)
{
	return (ctx->memsegs[VM_MEMSEG_LOW].size);
}

vm_paddr_t
vm_get_highmem_base(struct vmctx *ctx __unused)
{
	return (VM_HIGHMEM_BASE);
}

size_t
vm_get_highmem_size(struct vmctx *ctx)
{
	return (ctx->memsegs[VM_MEMSEG_HIGH].size);
}

static int
vm_get_regs(struct vcpu *vcpu, struct kvm_regs *regs)
{
	return (kvm_ioctl(vcpu->fd, KVM_GET_REGS, regs) < 0 ? errno : 0);
}

static int
vm_put_regs(struct vcpu *vcpu, struct kvm_regs *regs)
{
	return (kvm_ioctl(vcpu->fd, KVM_SET_REGS, regs) < 0 ? errno : 0);
}

int
vm_set_register(struct vcpu *vcpu, int reg, uint64_t val)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	__u64 *rp;
	int error;

	error = vm_get_regs(vcpu, &regs);
	if (error != 0)
		return (error);

	rp = NULL;
	switch (reg) {
	case VM_REG_GUEST_RAX: rp = &regs.rax; break;
	case VM_REG_GUEST_RBX: rp = &regs.rbx; break;
	case VM_REG_GUEST_RCX: rp = &regs.rcx; break;
	case VM_REG_GUEST_RDX: rp = &regs.rdx; break;
	case VM_REG_GUEST_RSI: rp = &regs.rsi; break;
	case VM_REG_GUEST_RDI: rp = &regs.rdi; break;
	case VM_REG_GUEST_RBP: rp = &regs.rbp; break;
	case VM_REG_GUEST_RSP: rp = &regs.rsp; break;
	case VM_REG_GUEST_R8: rp = &regs.r8; break;
	case VM_REG_GUEST_R9: rp = &regs.r9; break;
	case VM_REG_GUEST_R10: rp = &regs.r10; break;
	case VM_REG_GUEST_R11: rp = &regs.r11; break;
	case VM_REG_GUEST_R12: rp = &regs.r12; break;
	case VM_REG_GUEST_R13: rp = &regs.r13; break;
	case VM_REG_GUEST_R14: rp = &regs.r14; break;
	case VM_REG_GUEST_R15: rp = &regs.r15; break;
	case VM_REG_GUEST_RIP: rp = &regs.rip; break;
	case VM_REG_GUEST_RFLAGS: rp = &regs.rflags; break;
	default:
		break;
	}
	if (rp != NULL) {
		*rp = val;
		return (vm_put_regs(vcpu, &regs));
	}

	if (kvm_ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
		return (errno);
	switch (reg) {
	case VM_REG_GUEST_CR0: sregs.cr0 = val; break;
	case VM_REG_GUEST_CR2: sregs.cr2 = val; break;
	case VM_REG_GUEST_CR3: sregs.cr3 = val; break;
	case VM_REG_GUEST_CR4: sregs.cr4 = val; break;
	case VM_REG_GUEST_EFER: sregs.efer = val; break;
	default:
		return (EINVAL);
	}
	return (kvm_ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0 ? errno : 0);
}

int
vm_get_register(struct vcpu *vcpu, int reg, uint64_t *retval)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	int error;

	error = vm_get_regs(vcpu, &regs);
	if (error != 0)
		return (error);

	switch (reg) {
	case VM_REG_GUEST_RAX: *retval = regs.rax; return (0);
	case VM_REG_GUEST_RBX: *retval = regs.rbx; return (0);
	case VM_REG_GUEST_RCX: *retval = regs.rcx; return (0);
	case VM_REG_GUEST_RDX: *retval = regs.rdx; return (0);
	case VM_REG_GUEST_RSI: *retval = regs.rsi; return (0);
	case VM_REG_GUEST_RDI: *retval = regs.rdi; return (0);
	case VM_REG_GUEST_RBP: *retval = regs.rbp; return (0);
	case VM_REG_GUEST_RSP: *retval = regs.rsp; return (0);
	case VM_REG_GUEST_R8: *retval = regs.r8; return (0);
	case VM_REG_GUEST_R9: *retval = regs.r9; return (0);
	case VM_REG_GUEST_R10: *retval = regs.r10; return (0);
	case VM_REG_GUEST_R11: *retval = regs.r11; return (0);
	case VM_REG_GUEST_R12: *retval = regs.r12; return (0);
	case VM_REG_GUEST_R13: *retval = regs.r13; return (0);
	case VM_REG_GUEST_R14: *retval = regs.r14; return (0);
	case VM_REG_GUEST_R15: *retval = regs.r15; return (0);
	case VM_REG_GUEST_RIP: *retval = regs.rip; return (0);
	case VM_REG_GUEST_RFLAGS: *retval = regs.rflags; return (0);
	default:
		break;
	}

	if (kvm_ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
		return (errno);
	switch (reg) {
	case VM_REG_GUEST_CR0: *retval = sregs.cr0; return (0);
	case VM_REG_GUEST_CR2: *retval = sregs.cr2; return (0);
	case VM_REG_GUEST_CR3: *retval = sregs.cr3; return (0);
	case VM_REG_GUEST_CR4: *retval = sregs.cr4; return (0);
	case VM_REG_GUEST_EFER: *retval = sregs.efer; return (0);
	default:
		return (EINVAL);
	}
}

int
vm_set_register_set(struct vcpu *vcpu, unsigned int count, const int *regnums,
    uint64_t *regvals)
{
	for (unsigned int i = 0; i < count; i++) {
		int error = vm_set_register(vcpu, regnums[i], regvals[i]);
		if (error != 0)
			return (error);
	}
	return (0);
}

int
vm_get_register_set(struct vcpu *vcpu, unsigned int count, const int *regnums,
    uint64_t *regvals)
{
	for (unsigned int i = 0; i < count; i++) {
		int error = vm_get_register(vcpu, regnums[i], &regvals[i]);
		if (error != 0)
			return (error);
	}
	return (0);
}

static void
kvm_set_real_seg(struct kvm_segment *seg, uint16_t selector, uint64_t base,
    uint32_t type)
{
	memset(seg, 0, sizeof(*seg));
	seg->selector = selector;
	seg->base = base;
	seg->limit = 0xffff;
	seg->type = type;
	seg->present = 1;
	seg->s = 1;
}

int
vcpu_reset(struct vcpu *vcpu)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	if (kvm_ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
		return (errno);

	kvm_set_real_seg(&sregs.cs, 0xf000, 0xffff0000ULL, 0xb);
	kvm_set_real_seg(&sregs.ds, 0, 0, 0x3);
	kvm_set_real_seg(&sregs.es, 0, 0, 0x3);
	kvm_set_real_seg(&sregs.ss, 0, 0, 0x3);
	kvm_set_real_seg(&sregs.fs, 0, 0, 0x3);
	kvm_set_real_seg(&sregs.gs, 0, 0, 0x3);
	sregs.cr0 = 0x60000010;
	sregs.cr2 = 0;
	sregs.cr3 = 0;
	sregs.cr4 = 0;
	sregs.efer = 0;
	if (kvm_ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
		return (errno);

	memset(&regs, 0, sizeof(regs));
	regs.rip = 0xfff0;
	regs.rflags = 0x2;
	return (vm_put_regs(vcpu, &regs));
}

static int
kvm_handle_io(struct vcpu *vcpu)
{
	struct kvm_run *run;
	uint8_t *data;
	size_t stride;

	run = vcpu->run;
	data = (uint8_t *)run + run->io.data_offset;
	stride = run->io.size;
	for (uint32_t i = 0; i < run->io.count; i++) {
		struct vm_exit vme;
		uint32_t eax;
		int error;

		memset(&vme, 0, sizeof(vme));
		vme.exitcode = VM_EXITCODE_INOUT;
		vme.u.inout.bytes = run->io.size;
		vme.u.inout.in = run->io.direction == KVM_EXIT_IO_IN;
		vme.u.inout.port = run->io.port;
		if (!vme.u.inout.in)
			memcpy(&vme.u.inout.eax, data + i * stride, stride);

		error = emulate_inout(vcpu->ctx, vcpu, &vme);
		if (error != 0)
			return (error);

		if (vme.u.inout.in) {
			eax = vme.u.inout.eax;
			memcpy(data + i * stride, &eax, stride);
		}
	}
	return (0);
}

static int
kvm_handle_mmio(struct vcpu *vcpu)
{
	struct kvm_run *run;
	uint64_t val;

	run = vcpu->run;
	val = 0;
	if (run->mmio.is_write) {
		memcpy(&val, run->mmio.data, run->mmio.len);
		return (write_mem(vcpu, run->mmio.phys_addr, val,
		    run->mmio.len));
	}

	if (read_mem(vcpu, run->mmio.phys_addr, &val, run->mmio.len) != 0)
		return (-1);
	memcpy(run->mmio.data, &val, run->mmio.len);
	return (0);
}

int
vm_run(struct vcpu *vcpu, struct vm_run *vmrun)
{
	struct vm_exit *vme;

	vme = vmrun->vm_exit;
	for (;;) {
		if (kvm_ioctl(vcpu->fd, KVM_RUN, NULL) < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}

		if (vcpu->ctx->suspend_reason != VM_SUSPEND_NONE) {
			vme->exitcode = VM_EXITCODE_SUSPENDED;
			vme->u.suspended.how = vcpu->ctx->suspend_reason;
			return (0);
		}

		switch (vcpu->run->exit_reason) {
		case KVM_EXIT_IO:
			if (kvm_handle_io(vcpu) != 0)
				return (-1);
			break;
		case KVM_EXIT_MMIO:
			if (kvm_handle_mmio(vcpu) != 0)
				return (-1);
			break;
		case KVM_EXIT_HLT:
			vme->exitcode = VM_EXITCODE_HLT;
			vme->rip = vcpu->run->hw.hardware_exit_reason;
			return (0);
		case KVM_EXIT_SHUTDOWN:
			vme->exitcode = VM_EXITCODE_SUSPENDED;
			vme->u.suspended.how = VM_SUSPEND_TRIPLEFAULT;
			return (0);
		case KVM_EXIT_INTERNAL_ERROR:
		case KVM_EXIT_FAIL_ENTRY:
			return (-1);
		default:
			warnx("unexpected KVM exit reason %u",
			    vcpu->run->exit_reason);
			return (-1);
		}
	}
}

int
vm_suspend(struct vmctx *ctx, enum vm_suspend_how how)
{
	ctx->suspend_reason = how;
	return (0);
}

int
vm_reinit(struct vmctx *ctx __unused)
{
	return (0);
}

int
vm_raise_msi(struct vmctx *ctx, uint64_t addr, uint64_t msg, int bus __unused,
    int slot __unused, int func __unused)
{
	struct kvm_msi msi;

	memset(&msi, 0, sizeof(msi));
	msi.address_lo = addr;
	msi.address_hi = addr >> 32;
	msi.data = msg;
	return (kvm_ioctl(ctx->vm_fd, KVM_SIGNAL_MSI, &msi) < 0 ? errno : 0);
}

int
vm_apicid2vcpu(struct vmctx *ctx __unused, int apicid)
{
	return (apicid);
}

int
vm_ioapic_assert_irq(struct vmctx *ctx, int irq)
{
	struct kvm_irq_level level = { .irq = irq, .level = 1 };

	return (kvm_ioctl(ctx->vm_fd, KVM_IRQ_LINE, &level) < 0 ? errno : 0);
}

int
vm_ioapic_deassert_irq(struct vmctx *ctx, int irq)
{
	struct kvm_irq_level level = { .irq = irq, .level = 0 };

	return (kvm_ioctl(ctx->vm_fd, KVM_IRQ_LINE, &level) < 0 ? errno : 0);
}

int
vm_ioapic_pulse_irq(struct vmctx *ctx, int irq)
{
	int error;

	error = vm_ioapic_assert_irq(ctx, irq);
	if (error != 0)
		return (error);
	return (vm_ioapic_deassert_irq(ctx, irq));
}

int
vm_ioapic_pincount(struct vmctx *ctx __unused, int *pincount)
{
	*pincount = KVM_IOAPIC_PINS;
	return (0);
}

int
vm_isa_assert_irq(struct vmctx *ctx __unused, int atpic_irq __unused,
    int ioapic_irq __unused)
{
	return (EOPNOTSUPP);
}

int
vm_isa_deassert_irq(struct vmctx *ctx __unused, int atpic_irq __unused,
    int ioapic_irq __unused)
{
	return (EOPNOTSUPP);
}

int
vm_isa_pulse_irq(struct vmctx *ctx __unused, int atpic_irq __unused,
    int ioapic_irq __unused)
{
	return (EOPNOTSUPP);
}

int
vm_isa_set_irq_trigger(struct vmctx *ctx __unused, int atpic_irq __unused,
    enum vm_intr_trigger trigger __unused)
{
	return (EOPNOTSUPP);
}

int
vm_lapic_msi(struct vmctx *ctx, uint64_t addr, uint64_t msg)
{
	return (vm_raise_msi(ctx, addr, msg, 0, 0, 0));
}

int
vm_lapic_irq(struct vcpu *vcpu __unused, int vector __unused)
{
	return (EOPNOTSUPP);
}

int
vm_lapic_local_irq(struct vcpu *vcpu __unused, int vector __unused)
{
	return (EOPNOTSUPP);
}

int
vm_inject_nmi(struct vcpu *vcpu __unused)
{
	return (EOPNOTSUPP);
}

int
vm_inject_exception(struct vcpu *vcpu __unused, int vector __unused,
    int errcode_valid __unused, uint32_t errcode __unused,
    int restart_instruction __unused)
{
	return (EOPNOTSUPP);
}

void
vm_inject_fault(struct vcpu *vcpu __unused, int vector __unused,
    int errcode_valid __unused, int errcode __unused)
{
}

void
vm_inject_pf(struct vcpu *vcpu __unused, int error_code __unused,
    uint64_t cr2 __unused)
{
}

int
vm_readwrite_kernemu_device(struct vcpu *vcpu __unused, vm_paddr_t gpa __unused,
    bool write __unused, int size __unused, uint64_t *value __unused)
{
	return (EOPNOTSUPP);
}

int
vm_get_x2apic_state(struct vcpu *vcpu __unused, enum x2apic_state *s)
{
	*s = X2APIC_DISABLED;
	return (0);
}

int
vm_set_x2apic_state(struct vcpu *vcpu __unused, enum x2apic_state s)
{
	return (s == X2APIC_DISABLED ? 0 : EOPNOTSUPP);
}

int
vm_get_hpet_capabilities(struct vmctx *ctx __unused, uint32_t *capabilities)
{
	*capabilities = 0;
	return (0);
}

int
vm_capability_name2type(const char *capname __unused)
{
	return (-1);
}

const char *
vm_capability_type2name(int type __unused)
{
	return (NULL);
}

int
vm_get_capability(struct vcpu *vcpu, enum vm_cap_type cap, int *retval)
{
	switch (cap) {
	case VM_CAP_HALT_EXIT:
	case VM_CAP_PAUSE_EXIT:
	case VM_CAP_UNRESTRICTED_GUEST:
	case VM_CAP_ENABLE_INVPCID:
	case VM_CAP_IPI_EXIT:
		*retval = 1;
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

int
vm_set_capability(struct vcpu *vcpu __unused, enum vm_cap_type cap __unused,
    int val __unused)
{
	return (0);
}

int
vm_get_gpa_pmap(struct vmctx *ctx __unused, uint64_t gpa __unused,
    uint64_t *pte __unused, int *num __unused)
{
	return (EOPNOTSUPP);
}

int
vm_gla2gpa(struct vcpu *vcpu __unused, struct vm_guest_paging *paging __unused,
    uint64_t gla __unused, int prot __unused, uint64_t *gpa __unused,
    int *fault __unused)
{
	return (EOPNOTSUPP);
}

int
vm_gla2gpa_nofault(struct vcpu *vcpu __unused,
    struct vm_guest_paging *paging __unused, uint64_t gla __unused,
    int prot __unused, uint64_t *gpa __unused, int *fault __unused)
{
	return (EOPNOTSUPP);
}

int
vm_copy_setup(struct vcpu *vcpu __unused, struct vm_guest_paging *pg __unused,
    uint64_t gla __unused, size_t len __unused, int prot __unused,
    struct iovec *iov __unused, int iovcnt __unused, int *fault __unused)
{
	return (EOPNOTSUPP);
}

void
vm_copy_teardown(struct iovec *iov __unused, int iovcnt __unused)
{
}

void
vm_copyin(struct iovec *iov, void *vp, size_t len)
{
	char *dst = vp;

	while (len != 0) {
		size_t n = MIN(len, iov->iov_len);
		memcpy(dst, iov->iov_base, n);
		dst += n;
		len -= n;
		iov++;
	}
}

void
vm_copyout(const void *vp, struct iovec *iov, size_t len)
{
	const char *src = vp;

	while (len != 0) {
		size_t n = MIN(len, iov->iov_len);
		memcpy(iov->iov_base, src, n);
		src += n;
		len -= n;
		iov++;
	}
}

int
vm_get_intinfo(struct vcpu *vcpu __unused, uint64_t *i1 __unused,
    uint64_t *i2 __unused)
{
	return (EOPNOTSUPP);
}

int
vm_set_intinfo(struct vcpu *vcpu __unused, uint64_t exit_intinfo __unused)
{
	return (EOPNOTSUPP);
}

uint64_t *
vm_get_stats(struct vcpu *vcpu __unused, struct timeval *ret_tv __unused,
    int *ret_entries __unused)
{
	return (NULL);
}

const char *
vm_get_stat_desc(struct vmctx *ctx __unused, int index __unused)
{
	return (NULL);
}

static int
vm_get_cpus(struct vmctx *ctx, int which, cpuset_t *cpus)
{
	switch (which) {
	case VM_ACTIVE_CPUS:
		memcpy(cpus, &ctx->active_cpus, sizeof(*cpus));
		return (0);
	case VM_SUSPENDED_CPUS:
		memcpy(cpus, &ctx->suspended_cpus, sizeof(*cpus));
		return (0);
	default:
		CPU_ZERO(cpus);
		return (0);
	}
}

int
vm_active_cpus(struct vmctx *ctx, cpuset_t *cpus)
{
	return (vm_get_cpus(ctx, VM_ACTIVE_CPUS, cpus));
}

int
vm_suspended_cpus(struct vmctx *ctx, cpuset_t *cpus)
{
	return (vm_get_cpus(ctx, VM_SUSPENDED_CPUS, cpus));
}

int
vm_debug_cpus(struct vmctx *ctx, cpuset_t *cpus)
{
	return (vm_get_cpus(ctx, VM_DEBUG_CPUS, cpus));
}

int
vm_activate_cpu(struct vcpu *vcpu)
{
	CPU_SET_ATOMIC(vcpu->vcpuid, &vcpu->ctx->active_cpus);
	CPU_CLR_ATOMIC(vcpu->vcpuid, &vcpu->ctx->suspended_cpus);
	return (0);
}

int
vm_suspend_cpu(struct vcpu *vcpu)
{
	CPU_CLR_ATOMIC(vcpu->vcpuid, &vcpu->ctx->active_cpus);
	CPU_SET_ATOMIC(vcpu->vcpuid, &vcpu->ctx->suspended_cpus);
	return (0);
}

int
vm_suspend_all_cpus(struct vmctx *ctx)
{
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (ctx->vcpus[i] != NULL)
			vm_suspend_cpu(ctx->vcpus[i]);
	}
	return (0);
}

int
vm_resume_cpu(struct vcpu *vcpu)
{
	return (vm_activate_cpu(vcpu));
}

int
vm_resume_all_cpus(struct vmctx *ctx)
{
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (ctx->vcpus[i] != NULL)
			vm_activate_cpu(ctx->vcpus[i]);
	}
	return (0);
}

int
vm_restart_instruction(struct vcpu *vcpu __unused)
{
	return (EOPNOTSUPP);
}

int
vm_set_topology(struct vmctx *ctx __unused, uint16_t sockets __unused,
    uint16_t cores __unused, uint16_t threads __unused,
    uint16_t maxcpus __unused)
{
	return (0);
}

int
vm_get_topology(struct vmctx *ctx __unused, uint16_t *sockets,
    uint16_t *cores, uint16_t *threads, uint16_t *maxcpus)
{
	if (sockets != NULL)
		*sockets = 1;
	if (cores != NULL)
		*cores = 1;
	if (threads != NULL)
		*threads = 1;
	if (maxcpus != NULL)
		*maxcpus = CPU_SETSIZE;
	return (0);
}

int
vm_get_spi_interrupt_range(uint32_t *base, uint32_t *count)
{
	if (base != NULL)
		*base = 0;
	if (count != NULL)
		*count = 0;
	return (-1);
}

int
vm_assign_pptdev(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused)
{
	return (EOPNOTSUPP);
}

int
vm_unassign_pptdev(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused)
{
	return (EOPNOTSUPP);
}

int
vm_map_pptdev_mmio(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, vm_paddr_t gpa __unused,
    size_t len __unused, vm_paddr_t hpa __unused)
{
	return (EOPNOTSUPP);
}

int
vm_unmap_pptdev_mmio(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, vm_paddr_t gpa __unused,
    size_t len __unused)
{
	return (EOPNOTSUPP);
}

int
vm_setup_pptdev_msi(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, uint64_t addr __unused,
    uint64_t msg __unused, int numvec __unused)
{
	return (EOPNOTSUPP);
}

int
vm_setup_pptdev_msix(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, int idx __unused,
    uint64_t addr __unused, uint64_t msg __unused,
    uint32_t vector_control __unused)
{
	return (EOPNOTSUPP);
}

int
vm_disable_pptdev_msix(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused)
{
	return (EOPNOTSUPP);
}

int
vm_rtc_write(struct vmctx *ctx __unused, int offset __unused,
    uint8_t value __unused)
{
	return (EOPNOTSUPP);
}

int
vm_rtc_read(struct vmctx *ctx __unused, int offset __unused, uint8_t *retval)
{
	*retval = 0;
	return (0);
}

int
vm_rtc_settime(struct vmctx *ctx __unused, time_t secs __unused)
{
	return (0);
}

int
vm_rtc_gettime(struct vmctx *ctx __unused, time_t *secs)
{
	*secs = time(NULL);
	return (0);
}
