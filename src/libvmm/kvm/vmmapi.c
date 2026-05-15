/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "libutil.h"
#include "mem.h"
#include "vmmapi.h"
#include "internal.h"

#define	MB	(1024 * 1024UL)
#define	GB	(1024 * 1024 * 1024UL)
#define	KVM_API_VERSION_EXPECTED	12

#ifdef __amd64__
#define	VM_LOWMEM_LIMIT	(3 * GB)
#else
#define	VM_LOWMEM_LIMIT	0
#endif
#define	VM_HIGHMEM_BASE	(4 * GB)

static int
scorpi_kvm_unimplemented(const char *func)
{
	errno = ENOSYS;
	EPRINTLN("%s is not implemented for KVM yet", func);
	return (-1);
}

static int kvm_set_mp_state(struct vcpu *vcpu, uint32_t state);

int
vm_create(const char *name)
{
	struct vmctx *ctx;

	ctx = vm_openf(name, VMMAPI_OPEN_CREATE);
	if (ctx == NULL)
		return (-1);
	vm_close(ctx);
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
	int api_version;
	int saved_errno;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return (NULL);

	ctx->name = name != NULL ? strdup(name) : NULL;
	ctx->kvm_fd = -1;
	ctx->vm_fd = -1;
	ctx->vgic_fd = -1;
	ctx->sockets = 1;
	ctx->cores = 1;
	ctx->threads = 1;
	ctx->maxcpus = MAX_VCPUS;

	ctx->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (ctx->kvm_fd < 0)
		goto fail;

	api_version = ioctl(ctx->kvm_fd, KVM_GET_API_VERSION, 0);
	if (api_version != KVM_API_VERSION_EXPECTED) {
		errno = EPROTONOSUPPORT;
		goto fail;
	}

	ctx->vm_fd = ioctl(ctx->kvm_fd, KVM_CREATE_VM, 0);
	if (ctx->vm_fd < 0)
		goto fail;

	return (ctx);

fail:
	saved_errno = errno;
	vm_close(ctx);
	errno = saved_errno;
	return (NULL);
}

void
vm_close(struct vmctx *ctx)
{
	if (ctx == NULL)
		return;

	for (int i = 0; i < ctx->num_mem_ranges; i++) {
		if (ctx->mem_ranges[i].owned)
			free(ctx->mem_ranges[i].object);
	}
	if (ctx->vgic_fd >= 0)
		close(ctx->vgic_fd);
	if (ctx->vm_fd >= 0)
		close(ctx->vm_fd);
	if (ctx->kvm_fd >= 0)
		close(ctx->kvm_fd);
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
	int saved_errno;

	if (ctx == NULL || vcpuid < 0 || vcpuid >= MAX_VCPUS) {
		errno = EINVAL;
		return (NULL);
	}

	vcpu = calloc(1, sizeof(*vcpu));
	if (vcpu == NULL)
		return (NULL);
	vcpu->ctx = ctx;
	vcpu->vcpuid = vcpuid;
	vcpu->fd = -1;

	if (ctx->vm_fd >= 0) {
		vcpu->fd = ioctl(ctx->vm_fd, KVM_CREATE_VCPU, vcpuid);
		if (vcpu->fd < 0) {
			saved_errno = errno;
			free(vcpu);
			errno = saved_errno;
			return (NULL);
		}
	}

	ctx->vcpus[vcpuid] = vcpu;
	return (vcpu);
}

void
vm_vcpu_close(struct vcpu *vcpu)
{
	if (vcpu == NULL)
		return;
	if (vcpu->ctx != NULL && vcpu->vcpuid >= 0 && vcpu->vcpuid < MAX_VCPUS)
		vcpu->ctx->vcpus[vcpu->vcpuid] = NULL;
	if (vcpu->run != NULL && vcpu->run_size != 0)
		munmap(vcpu->run, vcpu->run_size);
	if (vcpu->fd >= 0)
		close(vcpu->fd);
	free(vcpu);
}

int
vcpu_id(struct vcpu *vcpu)
{
	return (vcpu->vcpuid);
}

int
vm_vcpu_init(struct vcpu *vcpu)
{
	struct vmctx *ctx;
	void *run;
	int mmap_size;
	int saved_errno;

	if (vcpu == NULL || vcpu->ctx == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (vcpu->initialized)
		return (0);

	ctx = vcpu->ctx;
	if (ctx->vm_fd < 0 || ctx->kvm_fd < 0 || vcpu->fd < 0) {
		errno = ENODEV;
		return (-1);
	}

	mmap_size = ioctl(ctx->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size <= 0)
		goto fail;

	run = mmap(NULL, (size_t)mmap_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, vcpu->fd, 0);
	if (run == MAP_FAILED)
		goto fail;

	vcpu->run = run;
	vcpu->run_size = (size_t)mmap_size;
	if (kvm_arch_vcpu_init(vcpu) != 0)
		goto fail;
	vcpu->initialized = true;

	for (int reg = 0; reg < VM_REG_LAST; reg++) {
		if (!vcpu->reg_set[reg])
			continue;
		if (kvm_arch_set_register(vcpu, reg, vcpu->regs[reg]) != 0)
			goto fail;
	}

	return (0);

fail:
	saved_errno = errno;
	if (vcpu->run != NULL && vcpu->run_size != 0) {
		munmap(vcpu->run, vcpu->run_size);
		vcpu->run = NULL;
		vcpu->run_size = 0;
	}
	vcpu->initialized = false;
	errno = saved_errno;
	return (-1);
}

void
vm_vcpu_deinit(struct vcpu *vcpu __unused)
{
}

int
vcpu_reset(struct vcpu *vcpu)
{
#if defined(__aarch64__)
	if (kvm_arch_vcpu_init(vcpu) != 0)
		return (-1);
	return (kvm_set_mp_state(vcpu,
	    vcpu->vcpuid == 0 ? KVM_MP_STATE_RUNNABLE : KVM_MP_STATE_STOPPED));
#else
	(void)vcpu;
	return (scorpi_kvm_unimplemented(__func__));
#endif
}

int
vm_parse_memsize(const char *opt, size_t *ret_memsize)
{
	char *endptr;
	size_t optval;
	int error;

	optval = strtoul(opt, &endptr, 0);
	if (*opt != '\0' && *endptr == '\0') {
		if (optval < MB)
			optval *= MB;
		*ret_memsize = optval;
		error = 0;
	} else {
		error = expand_number(opt, (uint64_t *)ret_memsize);
	}

	return (error);
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

static bool
vm_mem_allocated(struct vmctx *ctx, uint64_t gpa)
{
	for (int i = 0; i < ctx->num_mem_ranges; i++) {
		uint64_t base = ctx->mem_ranges[i].gpa;
		uint64_t limit = base + ctx->mem_ranges[i].len;
		if (gpa >= base && gpa < limit)
			return (true);
	}
	return (false);
}

static int
vm_setup_memory_segment_internal(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uint64_t prot, uintptr_t *addr)
{
	struct kvm_userspace_memory_region region;
	struct kvm_mem_range *seg;
	void *object;
	bool owned;

	if ((gpa & PAGE_MASK) || (len & PAGE_MASK) || len == 0)
		return (EINVAL);
	for (uint64_t p = gpa; p < gpa + len; p += PAGE_SIZE) {
		if (vm_mem_allocated(ctx, p))
			return (0);
	}
	if (ctx->num_mem_ranges >= VM_MAX_MEMORY_SEGMENTS)
		return (E2BIG);

	if ((prot & PROT_DONT_ALLOCATE) != 0) {
		if (addr == NULL || *addr == 0)
			return (EINVAL);
		object = (void *)*addr;
		owned = false;
	} else {
		if (posix_memalign(&object, PAGE_SIZE, len) != 0)
			return (ENOMEM);
		memset(object, 0, len);
		if (addr != NULL)
			*addr = (uintptr_t)object;
		owned = true;
	}

	seg = &ctx->mem_ranges[ctx->num_mem_ranges];
	seg->gpa = gpa;
	seg->len = len;
	seg->object = object;
	seg->slot = ctx->num_mem_ranges;
	seg->owned = owned;

	if (ctx->vm_fd >= 0) {
		memset(&region, 0, sizeof(region));
		region.slot = (uint32_t)seg->slot;
		region.guest_phys_addr = gpa;
		region.memory_size = len;
		region.userspace_addr = (uintptr_t)object;
#ifdef KVM_MEM_READONLY
		if ((prot & PROT_WRITE) == 0)
			region.flags |= KVM_MEM_READONLY;
#endif
		if (ioctl(ctx->vm_fd, KVM_SET_USER_MEMORY_REGION,
		    &region) < 0) {
			int error = errno;
			if (owned)
				free(object);
			return (error);
		}
	}

	ctx->num_mem_ranges++;
	return (0);
}

int
vm_setup_bootrom_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uintptr_t *addr)
{
	return (vm_setup_memory_segment_internal(ctx, gpa, len,
	    PROT_READ | PROT_WRITE | PROT_EXEC, addr));
}

int
vm_setup_memory_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uint64_t prot, uintptr_t *addr)
{
	return (vm_setup_memory_segment_internal(ctx, gpa, len, prot, addr));
}

int
vm_setup_memory(struct vmctx *ctx, size_t memsize, enum vm_mmap_style vms)
{
	uintptr_t *addr;
	int error;
	const uint64_t prot = PROT_READ | PROT_WRITE | PROT_EXEC;

	assert(vms == VM_MMAP_NONE || vms == VM_MMAP_ALL);

	ctx->memsegs[VM_MEMSEG_LOW].size =
	    (memsize > VM_LOWMEM_LIMIT) ? VM_LOWMEM_LIMIT : memsize;
	ctx->memsegs[VM_MEMSEG_HIGH].size =
	    (memsize > VM_LOWMEM_LIMIT) ?
	    (memsize - ctx->memsegs[VM_MEMSEG_LOW].size) : 0;

	if (ctx->memsegs[VM_MEMSEG_HIGH].size > 0) {
		addr = (vms == VM_MMAP_ALL) ?
		    &ctx->memsegs[VM_MEMSEG_HIGH].addr : NULL;
		error = vm_setup_memory_segment_internal(ctx, VM_HIGHMEM_BASE,
		    ctx->memsegs[VM_MEMSEG_HIGH].size, prot, addr);
		if (error != 0)
			return (error);
	}

	if (ctx->memsegs[VM_MEMSEG_LOW].size > 0) {
		addr = (vms == VM_MMAP_ALL) ?
		    &ctx->memsegs[VM_MEMSEG_LOW].addr : NULL;
		error = vm_setup_memory_segment_internal(ctx, 0,
		    ctx->memsegs[VM_MEMSEG_LOW].size, prot, addr);
		if (error != 0)
			return (error);
	}

	return (0);
}

void *
vm_map_gpa(struct vmctx *ctx, vm_paddr_t gaddr, size_t len)
{
	for (int i = 0; i < ctx->num_mem_ranges; i++) {
		struct kvm_mem_range *seg = &ctx->mem_ranges[i];
		if (gaddr >= seg->gpa && gaddr + len <= seg->gpa + seg->len)
			return ((char *)seg->object + (gaddr - seg->gpa));
	}
	return (NULL);
}

vm_paddr_t
vm_rev_map_gpa(struct vmctx *ctx, void *addr)
{
	for (int i = 0; i < ctx->num_mem_ranges; i++) {
		struct kvm_mem_range *seg = &ctx->mem_ranges[i];
		if (addr >= seg->object &&
		    (char *)addr < (char *)seg->object + seg->len)
			return (seg->gpa + ((char *)addr - (char *)seg->object));
	}
	return ((vm_paddr_t)-1);
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

const char *
vm_get_name(struct vmctx *ctx)
{
	return (ctx->name);
}

int
vm_get_guestmem_from_ctx(struct vmctx *ctx __unused,
    char **guest_baseaddr __unused, size_t *lowmem_size __unused,
    size_t *highmem_size __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_get_memseg(struct vmctx *ctx __unused, int segid __unused,
    size_t *lenp __unused, char *namebuf __unused, size_t bufsize __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_mmap_getnext(struct vmctx *ctx __unused, vm_paddr_t *gpa __unused,
    int *segid __unused, vm_offset_t *segoff __unused, size_t *len __unused,
    int *prot __unused, int *flags __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_munmap_memseg(struct vmctx *ctx __unused, vm_paddr_t gpa __unused,
    size_t len __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_set_register(struct vcpu *vcpu, int reg, uint64_t val)
{
	if (reg < 0 || reg >= VM_REG_LAST)
		return (EINVAL);
	vcpu->regs[reg] = val;
	vcpu->reg_set[reg] = true;
	if (vcpu->initialized)
		return (kvm_arch_set_register(vcpu, reg, val));
	return (0);
}

int
vm_get_register(struct vcpu *vcpu, int reg, uint64_t *retval)
{
	if (reg < 0 || reg >= VM_REG_LAST)
		return (EINVAL);
	if (vcpu->initialized)
		return (kvm_arch_get_register(vcpu, reg, retval));
	*retval = vcpu->regs[reg];
	return (0);
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

static int
kvm_mmio_exit(struct vcpu *vcpu, struct kvm_run *run)
{
	uint64_t val;
	int error;

	if (run->mmio.len == 0 || run->mmio.len > sizeof(run->mmio.data)) {
		errno = EINVAL;
		return (-1);
	}

	if (run->mmio.is_write) {
		val = 0;
		memcpy(&val, run->mmio.data, run->mmio.len);
		error = write_mem(vcpu, run->mmio.phys_addr, val,
		    (int)run->mmio.len);
	} else {
		val = 0;
		error = read_mem(vcpu, run->mmio.phys_addr, &val,
		    (int)run->mmio.len);
		if (error == 0) {
			memset(run->mmio.data, 0, sizeof(run->mmio.data));
			memcpy(run->mmio.data, &val, run->mmio.len);
		}
	}

	if (error != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

static void
kvm_set_suspended_exit(struct vcpu *vcpu, struct vm_exit *vme,
    enum vm_suspend_how how)
{
	uint64_t pc;

	memset(vme, 0, sizeof(*vme));
	vme->exitcode = VM_EXITCODE_SUSPENDED;
	vme->u.suspended.how = how;
	if (vm_get_register(vcpu, VM_REG_GUEST_PC, &pc) == 0)
		vme->pc = pc;
}

static int
kvm_set_mp_state(struct vcpu *vcpu, uint32_t state)
{
	struct kvm_mp_state mp_state;

	if (vcpu == NULL || vcpu->fd < 0) {
		errno = EINVAL;
		return (-1);
	}

	memset(&mp_state, 0, sizeof(mp_state));
	mp_state.mp_state = state;
	return (ioctl(vcpu->fd, KVM_SET_MP_STATE, &mp_state));
}

static int
kvm_suspend_for_reset(struct vmctx *ctx, int current_vcpuid)
{
	(void)current_vcpuid;

	ctx->suspend_reason = VM_SUSPEND_RESET;
	return (0);
}

static int
kvm_system_event_exit(struct vcpu *vcpu, struct kvm_run *run,
    struct vm_exit *vme)
{
	switch (run->system_event.type) {
	case KVM_SYSTEM_EVENT_SHUTDOWN:
		kvm_set_suspended_exit(vcpu, vme, VM_SUSPEND_POWEROFF);
		return (0);
	case KVM_SYSTEM_EVENT_RESET:
		if (kvm_suspend_for_reset(vcpu->ctx, vcpu->vcpuid) != 0)
			return (-1);
		kvm_set_suspended_exit(vcpu, vme, VM_SUSPEND_RESET);
		return (0);
	case KVM_SYSTEM_EVENT_CRASH:
		kvm_set_suspended_exit(vcpu, vme, VM_SUSPEND_HALT);
		return (0);
	default:
		EPRINTLN("KVM system event %u is not handled",
		    run->system_event.type);
		errno = ENOTSUP;
		return (-1);
	}
}

int
vm_run(struct vcpu *vcpu, struct vm_run *vmrun)
{
	struct kvm_run *run;
	struct vm_exit *vme;

	if (vcpu == NULL || vmrun == NULL || vmrun->vm_exit == NULL ||
	    vcpu->fd < 0 || vcpu->run == NULL) {
		errno = EINVAL;
		return (-1);
	}

	run = vcpu->run;
	vme = vmrun->vm_exit;

	for (;;) {
		if (ioctl(vcpu->fd, KVM_RUN, 0) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return (-1);
		}

		switch (run->exit_reason) {
		case KVM_EXIT_MMIO:
			if (kvm_mmio_exit(vcpu, run) != 0)
				return (-1);
			break;
		case KVM_EXIT_INTR:
			break;
		case KVM_EXIT_HLT:
			kvm_set_suspended_exit(vcpu, vme, VM_SUSPEND_HALT);
			return (0);
		case KVM_EXIT_SYSTEM_EVENT:
			return (kvm_system_event_exit(vcpu, run, vme));
		case KVM_EXIT_FAIL_ENTRY:
			EPRINTLN("KVM fail entry: reason %#llx cpu %u",
			    run->fail_entry.hardware_entry_failure_reason,
			    run->fail_entry.cpu);
			errno = EIO;
			return (-1);
		case KVM_EXIT_INTERNAL_ERROR:
			EPRINTLN("KVM internal error: suberror %u ndata %u",
			    run->internal.suberror, run->internal.ndata);
			errno = EIO;
			return (-1);
		case KVM_EXIT_ARM_NISV:
			EPRINTLN("KVM ARM NISV exit: esr_iss %#llx ipa %#llx",
			    run->arm_nisv.esr_iss, run->arm_nisv.fault_ipa);
			errno = ENOTSUP;
			return (-1);
		default:
			EPRINTLN("Unhandled KVM exit reason %u",
			    run->exit_reason);
			errno = ENOTSUP;
			return (-1);
		}
	}
}

int
vm_suspend(struct vmctx *ctx, enum vm_suspend_how how)
{
	ctx->suspend_reason = how;
#if defined(__aarch64__)
	if (how == VM_SUSPEND_RESET)
		return (kvm_suspend_for_reset(ctx, -1));
#endif
	return (0);
}

int
vm_reinit(struct vmctx *ctx __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

#if defined(__aarch64__)
int
vm_raise_msi(struct vmctx *ctx, uint64_t addr, uint64_t msg, int bus, int slot,
    int func)
{
	return (kvm_arch_raise_msi(ctx, addr, msg, bus, slot, func));
}

int
vm_attach_vgic(struct vmctx *ctx __unused, uint64_t dist_start __unused,
    size_t dist_size __unused, uint64_t redist_start __unused,
    size_t redist_size __unused, uint64_t mmio_base __unused,
    uint32_t spi_intid_base __unused, uint32_t spi_intid_count __unused)
{
	return (kvm_arch_attach_vgic(ctx, dist_start, dist_size, redist_start,
	    redist_size, mmio_base, spi_intid_base, spi_intid_count));
}

int
vm_inject_exception(struct vcpu *vcpu __unused, uint64_t esr __unused,
    uint64_t far __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_get_spi_interrupt_range(uint32_t *spi_intid_base, uint32_t *spi_intid_count)
{
	if (spi_intid_base != NULL)
		*spi_intid_base = 32;
	if (spi_intid_count != NULL)
		*spi_intid_count = 224;
	return (0);
}

int
vm_assert_irq(struct vmctx *ctx, uint32_t irq)
{
	return (kvm_arch_set_irq(ctx, irq, true));
}

int
vm_deassert_irq(struct vmctx *ctx, uint32_t irq)
{
	return (kvm_arch_set_irq(ctx, irq, false));
}
#else
int
vm_raise_msi(struct vmctx *ctx __unused, uint64_t addr __unused,
    uint64_t msg __unused, int bus __unused, int slot __unused,
    int func __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}
#endif

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
vm_get_capability(struct vcpu *vcpu __unused, enum vm_cap_type cap,
    int *retval)
{
	switch (cap) {
	case VM_CAP_HALT_EXIT:
	case VM_CAP_PAUSE_EXIT:
	case VM_CAP_MASK_HWINTR:
		*retval = 0;
		return (0);
	case VM_CAP_UNRESTRICTED_GUEST:
		*retval = 1;
		return (0);
	default:
		break;
	}
	return (-1);
}

int
vm_set_capability(struct vcpu *vcpu __unused, enum vm_cap_type cap __unused,
    int val __unused)
{
	return (0);
}

int
vm_assign_pptdev(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_unassign_pptdev(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_map_pptdev_mmio(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, vm_paddr_t gpa __unused,
    size_t len __unused, vm_paddr_t hpa __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_unmap_pptdev_mmio(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, vm_paddr_t gpa __unused,
    size_t len __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_setup_pptdev_msi(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, uint64_t addr __unused,
    uint64_t msg __unused, int numvec __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_setup_pptdev_msix(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused, int idx __unused,
    uint64_t addr __unused, uint64_t msg __unused,
    uint32_t vector_control __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_disable_pptdev_msix(struct vmctx *ctx __unused, int bus __unused,
    int slot __unused, int func __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_get_intinfo(struct vcpu *vcpu __unused, uint64_t *info1 __unused,
    uint64_t *info2 __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_set_intinfo(struct vcpu *vcpu __unused, uint64_t info1 __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
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

void
vm_copy_teardown(struct iovec *iov __unused, int iovcnt __unused)
{
}

#ifndef min
#define	min(a, b)	(((a) < (b)) ? (a) : (b))
#endif

void
vm_copyin(struct iovec *iov, void *vp, size_t len)
{
	const char *src;
	char *dst;
	size_t n;

	dst = vp;
	while (len) {
		assert(iov->iov_len);
		n = min(len, iov->iov_len);
		src = iov->iov_base;
		memcpy(dst, src, n);
		iov++;
		dst += n;
		len -= n;
	}
}

void
vm_copyout(const void *vp, struct iovec *iov, size_t len)
{
	const char *src;
	char *dst;
	size_t n;

	src = vp;
	while (len) {
		assert(iov->iov_len);
		n = min(len, iov->iov_len);
		dst = iov->iov_base;
		memcpy(dst, src, n);
		iov++;
		src += n;
		len -= n;
	}
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
		return (-1);
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
	if (SCORPI_CPU_ISSET((unsigned)vcpu->vcpuid, &vcpu->ctx->active_cpus))
		return (EBUSY);
	SCORPI_CPU_CLR_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->suspended_cpus);
	SCORPI_CPU_SET_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->active_cpus);
	return (0);
}

int
vm_suspend_all_cpus(struct vmctx *ctx)
{
	for (int vcpuid = 0; vcpuid < MAX_VCPUS; vcpuid++) {
		if (ctx->vcpus[vcpuid] != NULL)
			vm_suspend_cpu(ctx->vcpus[vcpuid]);
	}
	return (0);
}

int
vm_suspend_cpu(struct vcpu *vcpu)
{
	SCORPI_CPU_CLR_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->active_cpus);
	SCORPI_CPU_SET_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->suspended_cpus);
	return (0);
}

int
vm_resume_all_cpus(struct vmctx *ctx)
{
	for (int vcpuid = 0; vcpuid < MAX_VCPUS; vcpuid++) {
		if (ctx->vcpus[vcpuid] != NULL)
			vm_resume_cpu(ctx->vcpus[vcpuid]);
	}
	return (0);
}

int
vm_resume_cpu(struct vcpu *vcpu)
{
	return (vm_activate_cpu(vcpu));
}

#if defined(__aarch64__)
bool
vm_uses_in_kernel_psci(struct vmctx *ctx __unused)
{
	return (true);
}
#endif

int
vm_restart_instruction(struct vcpu *vcpu __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_set_topology(struct vmctx *ctx, uint16_t sockets, uint16_t cores,
    uint16_t threads, uint16_t maxcpus)
{
	ctx->sockets = sockets;
	ctx->cores = cores;
	ctx->threads = threads;
	ctx->maxcpus = maxcpus != 0 ? maxcpus : MAX_VCPUS;
	return (0);
}

int
vm_get_topology(struct vmctx *ctx, uint16_t *sockets, uint16_t *cores,
    uint16_t *threads, uint16_t *maxcpus)
{
	*sockets = ctx->sockets;
	*cores = ctx->cores;
	*threads = ctx->threads;
	*maxcpus = ctx->maxcpus;
	return (0);
}

int
vm_snapshot_req(struct vmctx *ctx __unused,
    struct vm_snapshot_meta *meta __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_restore_time(struct vmctx *ctx __unused)
{
	return (0);
}

#if defined(__amd64__)
int
vm_get_gpa_pmap(struct vmctx *ctx __unused, uint64_t gpa __unused,
    uint64_t *pte __unused, int *num __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_gla2gpa(struct vcpu *vcpu __unused, struct vm_guest_paging *paging __unused,
    uint64_t gla __unused, int prot __unused, uint64_t *gpa __unused,
    int *fault __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_get_x2apic_state(struct vcpu *vcpu __unused, enum x2apic_state *s __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_set_x2apic_state(struct vcpu *vcpu __unused, enum x2apic_state s __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_get_hpet_capabilities(struct vmctx *ctx __unused,
    uint32_t *capabilities __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_copy_setup(struct vcpu *vcpu __unused,
    struct vm_guest_paging *paging __unused, uint64_t gla __unused,
    size_t len __unused, int prot __unused, struct iovec *iov __unused,
    int iovcnt __unused, int *fault __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_rtc_write(struct vmctx *ctx __unused, int offset __unused,
    uint8_t value __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_rtc_read(struct vmctx *ctx __unused, int offset __unused,
    uint8_t *retval __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_rtc_settime(struct vmctx *ctx __unused, time_t secs __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}

int
vm_rtc_gettime(struct vmctx *ctx __unused, time_t *secs __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}
#endif

int
vm_gla2gpa_nofault(struct vcpu *vcpu __unused,
    struct vm_guest_paging *paging __unused, uint64_t gla __unused,
    int prot __unused, uint64_t *gpa __unused, int *fault __unused)
{
	return (scorpi_kvm_unimplemented(__func__));
}
