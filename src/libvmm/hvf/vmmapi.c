/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
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
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <Hypervisor/Hypervisor.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <sys/param.h>
#include <pthread.h>
// #include <sys/sysctl.h>
// #include <sys/ioctl.h>
#include <sys/mman.h>
// #include <support/linker.h>
// #include <sys/module.h>
#include <sys/uio.h>
#include <support/cpuset.h>

// #include <capsicum_helpers.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libutil.h"

// #include <vm/vm.h>
#include "vmm.h"
#ifdef WITH_VMMAPI_SNAPSHOT
#include <machine/vmm_snapshot.h>
#endif

#include "arch.h"
#include "debug.h"
#include "internal.h"
#include "vmmapi.h"

#if defined(VMM_DEBUG)
#define DPRINTLN PRINTLN
#else
#define DPRINTLN(format, arg...)
#endif

#define MB (1024 * 1024UL)
#define GB (1024 * 1024 * 1024UL)

#ifdef __amd64__
#define VM_LOWMEM_LIMIT (3 * GB)
#else
#define VM_LOWMEM_LIMIT 0
#endif
#define VM_HIGHMEM_BASE (4 * GB)

/*
 * Size of the guard region before and after the virtual address space
 * mapping the guest physical memory. This must be a multiple of the
 * superpage size for performance reasons.
 */
#define VM_MMAP_GUARD_SIZE (4 * MB)

BITSET_DEFINE(regs_mask, 32);
#define REGMASK_SIZE 32

struct vcpu {
	struct vmctx *ctx;
	int vcpuid;

	hv_vcpu_t vcpu;
	hv_vcpu_exit_t *vcpu_exit;

	pthread_t tid;

	uint64_t regs[REGMASK_SIZE];
	struct regs_mask regmask;

	uint64_t pmcr_el0;
	uint64_t pmcntenset_el0;
	uint64_t pmintenset_el1;
	uint64_t pmovsclr_el0;
	uint64_t pmselr_el0;
	uint64_t pmuserenr_el0;
	uint64_t pmccfiltr_el0;
	uint64_t pmxevtyper_el0;
	uint64_t oslar_el1;
	uint64_t osdlr_el1;
};

static int guest_vaddr2paddr(struct vcpu *vcpu, uint64_t vaddr,
    uint64_t *paddr);

int
vm_create(const char *name)
{
	exit(-1);
	return (-1);
}

struct vmctx *
vm_open(const char *name)
{
	return (NULL);
}

void
vm_close(struct vmctx *vm)
{
}

void
vm_destroy(struct vmctx *vm)
{
	vm_close(vm);
}

struct vcpu *
vm_vcpu_open(struct vmctx *ctx, int vcpuid)
{
	struct vcpu *vcpu;

	vcpu = calloc(1, sizeof(*vcpu));
	if (vcpu == NULL)
		return (NULL);
	vcpu->ctx = ctx;
	vcpu->vcpuid = vcpuid;
	BIT_ZERO(REGMASK_SIZE, &vcpu->regmask);

	ctx->vcpus[vcpuid] = vcpu;

	return (vcpu);
}

#ifdef VCPU_WATCHDOG
pthread_t watchdog_tid;
static atomic_int watchdog_var[128];
static int kicked = 0;

void *
watchdog_thread(void *arg)
{
	int i;

	for (i = 0; i < nitems(watchdog_var); i++) {
		atomic_store(&watchdog_var[i], 0);
	}

	while (1) {
		sleep(5);
		for (i = 0; i < nitems(watchdog_var); i++) {
			int v = atomic_load(&watchdog_var[i]);
			if (v == 1) {
				EPRINTLN("vcpu %d: hang condition detected", i);
				hv_vcpu_t cpus[1] = { i };
				hv_vcpus_exit(cpus, 1);
				kicked = 1;
			} else if (v > 1) {
				atomic_store(&watchdog_var[i], 1);
			}
		}
	}
	return NULL;
}

void
vm_create_watchdog(void)
{
	if (!watchdog_tid) {
		pthread_create(&watchdog_tid, NULL, watchdog_thread, NULL);
	}
}
#endif

int
vm_vcpu_init(struct vcpu *vcpu)
{
	uint64_t fr0, pfr0;
	if (HV_SUCCESS != hv_vcpu_create(&vcpu->vcpu, &vcpu->vcpu_exit, NULL)) {
		EPRINTLN("vm_vcpu_init() failed");
		free(vcpu);
		return (-1);
	}
	vcpu->tid = pthread_self();

	hv_vcpu_set_trap_debug_exceptions(vcpu->vcpu, false);
	hv_vcpu_set_trap_debug_reg_accesses(vcpu->vcpu, false);

	// sets affinity, otherwise gic redistributor is not defined
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_MPIDR_EL1, vcpu_id(vcpu));

	// start in exception mode with interrupts disabled
	hv_vcpu_set_reg(vcpu->vcpu, HV_REG_CPSR, 0x3c5);

	// If starting in AArch64 state, the SPSR_ELx.{D,A,I,F} bits must be set
	// to {1, 1, 1, 1}.
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_SPSR_EL1, 0x5C0);

	//

	// hv_vcpu_set_sys_reg(vcpu->vcpu,  HV_SYS_REG_SCTLR_EL1, 0x30100180);

	/*
	It took me almost week to discover why vcpu_run() would hang in the
	beginning of executing the Windows cdboot.efi loader. After
	dissasembling and analyzing the efi code I realized this rather
	innocuous instruction would cause it: mrs        x8, pmcr_el0 Then I
	started looking for a workaround. Apparently when the Apple vgic is not
	enabled the instruction would cause an exit with EXCP_MSR code, not so
	with the vgic enabled. F*ck you Apple!
	*/
	// Workaround: set PMU enabled
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, &fr0);
	fr0 |= (1 << 8); // Set PMUVer to 0b0001 for Windows guest
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, fr0);

	// set gicv3 feat
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_ID_AA64PFR0_EL1, &pfr0);
	pfr0 |= (1 << 24);
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_ID_AA64PFR0_EL1, pfr0);

#ifdef VCPU_WATCHDOG
	vm_create_watchdog();
#endif
	return (0);
}

void
vm_vcpu_deinit(struct vcpu *vcpu)
{
	hv_vcpu_destroy(vcpu->vcpu);
	vcpu->vcpu = 0;
}

int
vcpu_reset(struct vcpu *vcpu)
{
	if (vcpu_id(vcpu) == 0)
		hv_gic_reset();
	vm_vcpu_deinit(vcpu);
	return vm_vcpu_init(vcpu);
}

void
vm_vcpu_close(struct vcpu *vcpu)
{
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
	size_t optval;
	int error;

	optval = strtoul(opt, &endptr, 0);
	if (*opt != '\0' && *endptr == '\0') {
		/*
		 * For the sake of backward compatibility if the memory size
		 * specified on the command line is less than a megabyte then
		 * it is interpreted as being in units of MB.
		 */
		if (optval < MB)
			optval *= MB;
		*ret_memsize = optval;
		error = 0;
	} else
		error = expand_number(opt, (uint64_t *)ret_memsize);

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
	// ctx->memflags = flags;
}

int
vm_get_memflags(struct vmctx *ctx)
{
	// return (ctx->memflags);
	return -1;
}

/*
 * Map segment 'segid' starting at 'off' into guest address range [gpa,gpa+len).
 */
/*int
vm_mmap_memseg(struct vmctx *ctx, vm_paddr_t gpa, int segid, vm_offset_t off,
    size_t len, int prot)
{
	printf("vm_mmap_memseg");
    return (0);
}*/

int
vm_get_guestmem_from_ctx(struct vmctx *ctx, char **guest_baseaddr,
    size_t *lowmem_size, size_t *highmem_size)
{
	printf("vm_get_guestmem_from_ctx\n");
	return (0);
}

int
vm_munmap_memseg(struct vmctx *ctx, vm_paddr_t gpa, size_t len)
{
	printf("vm_munmap_memseg\n");
	return -1;
}

int
vm_mmap_getnext(struct vmctx *ctx, vm_paddr_t *gpa, int *segid,
    vm_offset_t *segoff, size_t *len, int *prot, int *flags)
{
	printf("vm_mmap_getnext\n");
	return -1;
}

bool
vm_mem_allocated(struct vmctx *ctx, uint64_t gpa)
{
	int i;
	uint64_t gpabase, gpalimit;

	for (i = 0; i < ctx->num_mem_ranges; i++) {
		gpabase = ctx->mem_ranges[i].gpa;
		gpalimit = gpabase + ctx->mem_ranges[i].len;
		if (gpa >= gpabase && gpa < gpalimit)
			return (true);
	}

	return (false);
}

static int
vm_malloc(struct vmctx *ctx, uint64_t gpa, size_t len, uint64_t prot,
    uintptr_t *addr)
{
	int available, allocated;
	struct mem_range *seg;
	void *object;
	uint64_t g;
	hv_memory_flags_t hvProt;

	if ((gpa & PAGE_MASK) || (len & PAGE_MASK) || len == 0)
		return (EINVAL);

	available = allocated = 0;
	g = gpa;
	while (g < gpa + len) {
		if (vm_mem_allocated(ctx, g))
			allocated++;
		else
			available++;

		g += PAGE_SIZE;
	}

	/*
	 * If there are some allocated and some available pages in the address
	 * range then it is an error.
	 */
	if (allocated && available)
		return (EINVAL);

	/*
	 * If the entire address range being requested has already been
	 * allocated then there isn't anything more to do.
	 */
	if (allocated && available == 0)
		return (0);

	if (ctx->num_mem_ranges >= VM_MAX_MEMORY_SEGMENTS)
		return (E2BIG);

	seg = &ctx->mem_ranges[ctx->num_mem_ranges];

	if (prot & PROT_DONT_ALLOCATE) {
		object = (void *)*addr;
	} else {
		if (posix_memalign(&object, PAGE_SIZE, len)) {
			EPRINTLN("posix_memalign() failed");
			return (ENOMEM);
		}
		*addr = (uintptr_t)object;
	}

	hvProt = (prot & PROT_READ) ? HV_MEMORY_READ : 0;
	hvProt |= (prot & PROT_WRITE) ? HV_MEMORY_WRITE : 0;
	hvProt |= (prot & PROT_EXEC) ? HV_MEMORY_EXEC : 0;

	if (hv_vm_map(object, gpa, len, hvProt)) {
		EPRINTLN("hv_vm_map() failed");
		if (!(prot & PROT_DONT_ALLOCATE))
			free(object);
		return (ENOMEM);
	}

	seg->gpa = gpa;
	seg->len = len;
	seg->object = object;

	ctx->num_mem_ranges++;

	return (0);
}

int
get_memobj(struct vmctx *ctx, uint64_t gpa, size_t len, uint64_t *offset,
    void **object)
{
	int i;
	size_t seg_len;
	uint64_t seg_gpa;
	void *seg_obj;

	for (i = 0; i < ctx->num_mem_ranges; i++) {
		if ((seg_obj = ctx->mem_ranges[i].object) == NULL)
			continue;

		seg_gpa = ctx->mem_ranges[i].gpa;
		seg_len = ctx->mem_ranges[i].len;

		if ((gpa >= seg_gpa) && ((gpa + len) <= (seg_gpa + seg_len))) {
			*offset = gpa - seg_gpa;
			*object = seg_obj;
			return (0);
		}
	}

	return (EINVAL);
}

static int
setup_memory_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uint64_t prot, uintptr_t *addr)
{
	int error;

	// vcpu_freeze_all(true);
	error = vm_malloc(ctx, gpa, len, prot, addr);
	// vcpu_freeze_all(false);
	return (error);
}

int
vm_setup_bootrom_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uintptr_t *addr)
{
	int error;

	error = setup_memory_segment(ctx, gpa, len,
	    PROT_READ | PROT_WRITE | PROT_EXEC, addr);
	return (error);
}

int
vm_setup_memory_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uint64_t prot, uintptr_t *addr)
{
	return setup_memory_segment(ctx, gpa, len, prot, addr);
}

int
vm_get_memseg(struct vmctx *ctx, int segid, size_t *lenp, char *namebuf,
    size_t bufsize)
{
	printf("vm_get_memseg\n");
	return -1;
}

int
vm_setup_memory(struct vmctx *ctx, size_t memsize, enum vm_mmap_style vms)
{
	uintptr_t *addr;
	int error;
	const uint64_t protFlags = PROT_READ | PROT_WRITE | PROT_EXEC;

	assert(vms == VM_MMAP_NONE || vms == VM_MMAP_ALL);

	/*
	 * If 'len' cannot fit entirely in the 'lowmem' segment then
	 * create another 'highmem' segment above for the remainder.
	 */

	ctx->memsegs[VM_MEMSEG_LOW].size = (memsize > VM_LOWMEM_LIMIT) ?
	    VM_LOWMEM_LIMIT :
	    memsize;
	ctx->memsegs[VM_MEMSEG_HIGH].size = (memsize > VM_LOWMEM_LIMIT) ?
	    (memsize - ctx->memsegs[VM_MEMSEG_LOW].size) :
	    0;

	if (ctx->memsegs[VM_MEMSEG_HIGH].size > 0) {
		addr = (vms == VM_MMAP_ALL) ?
		    &ctx->memsegs[VM_MEMSEG_HIGH].addr :
		    NULL;
		if ((error = setup_memory_segment(ctx, VM_HIGHMEM_BASE,
			 ctx->memsegs[VM_MEMSEG_HIGH].size, protFlags, addr))) {
			return (error);
		}
	}

	if (ctx->memsegs[VM_MEMSEG_LOW].size > 0) {
		addr = (vms == VM_MMAP_ALL) ?
		    &ctx->memsegs[VM_MEMSEG_LOW].addr :
		    NULL;
		if ((error = setup_memory_segment(ctx, 0,
			 ctx->memsegs[VM_MEMSEG_LOW].size, protFlags, addr))) {
			return (error);
		}
	}

	return (0);
}

/*
 * Returns a non-NULL pointer if [gaddr, gaddr+len) is entirely contained in
 * the lowmem or highmem regions.
 *
 * In particular return NULL if [gaddr, gaddr+len) falls in guest MMIO region.
 * The instruction emulation code depends on this behavior.
 */
void *
vm_map_gpa(struct vmctx *ctx, vm_paddr_t gaddr, size_t len)
{
	vm_size_t lowsize, highsize;

	lowsize = ctx->memsegs[VM_MEMSEG_LOW].size;
	if (lowsize > 0) {
		if (gaddr < lowsize && len <= lowsize && gaddr + len <= lowsize)
			return (
			    (void *)ctx->memsegs[VM_MEMSEG_LOW].addr + gaddr);
	}

	highsize = ctx->memsegs[VM_MEMSEG_HIGH].size;
	if (highsize > 0 && gaddr >= VM_HIGHMEM_BASE) {
		if (gaddr < VM_HIGHMEM_BASE + highsize && len <= highsize &&
		    gaddr + len <= VM_HIGHMEM_BASE + highsize)
			return ((void *)ctx->memsegs[VM_MEMSEG_HIGH].addr +
			    (gaddr - VM_HIGHMEM_BASE));
	}

	return (NULL);
}

vm_paddr_t
vm_rev_map_gpa(struct vmctx *ctx, void *addr)
{
	printf("vm_rev_map_gpa\n");

	return ((vm_paddr_t)-1);
}

const char *
vm_get_name(struct vmctx *ctx)
{
	return (NULL);
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

int
vm_set_register(struct vcpu *vcpu, int reg, uint64_t val)
{
	if (vcpu->tid != pthread_self()) {
		if (reg == VM_REG_GUEST_PC) {
			reg = HV_REG_PC;
		}
		if (reg > 31) {
			EPRINTLN(
			    "vm_set_register: cannot set a register from a different thread");
			return (-1);
		}
		vcpu->regs[reg] = val;
		BIT_SET_ATOMIC(REGMASK_SIZE, reg, &vcpu->regmask);
		return (0);
	}
	if (reg >= VM_REG_GUEST_X0 && reg <= VM_REG_GUEST_X29) {
		return hv_vcpu_set_reg(vcpu->vcpu,
		    HV_REG_X0 + (reg - VM_REG_GUEST_X0), val);
	}
	switch (reg) {
	case VM_REG_GUEST_LR:
		return hv_vcpu_set_reg(vcpu->vcpu, HV_REG_LR, val);
	case VM_REG_GUEST_PC:
		return hv_vcpu_set_reg(vcpu->vcpu, HV_REG_PC, val);
	case VM_REG_GUEST_CPSR:
		return hv_vcpu_set_reg(vcpu->vcpu, HV_REG_CPSR, val);
		// case VM_REG_GUEST_SP:
		//	return hv_vcpu_set_sys_reg(vcpu->vcpu,
		//HV_SYS_REG_SP_EL1, val);
	}
	EPRINTLN("vm_set_register: bad register %d\n", reg);
	return (-1);
}

int
vm_get_register(struct vcpu *vcpu, int reg, uint64_t *ret_val)
{
	if (vcpu->tid != pthread_self()) {
		EPRINTLN(
		    "vm_get_register: cannot set a register from a different thread");
		return (-1);
	}
	if (reg >= VM_REG_GUEST_X0 && reg <= VM_REG_GUEST_X29) {
		return hv_vcpu_get_reg(vcpu->vcpu,
		    HV_REG_X0 + (reg - VM_REG_GUEST_X0), ret_val);
	}
	switch (reg) {
	case VM_REG_GUEST_LR:
		return hv_vcpu_get_reg(vcpu->vcpu, HV_REG_LR, ret_val);
	case VM_REG_GUEST_PC:
		return hv_vcpu_get_reg(vcpu->vcpu, HV_REG_PC, ret_val);
	case VM_REG_GUEST_CPSR:
		return hv_vcpu_get_reg(vcpu->vcpu, HV_REG_CPSR, ret_val);
	case VM_REG_GUEST_SP:
		*ret_val = 0;
		return 0; // wzr
	}
	EPRINTLN("vm_get_register: bad register %d\n", reg);
	return (-1);
}

int
vm_set_register_set(struct vcpu *vcpu, unsigned int count, const int *regnums,
    uint64_t *regvals)
{
	printf("vm_set_register_set\n");
	return -1;
}

int
vm_get_register_set(struct vcpu *vcpu, unsigned int count, const int *regnums,
    uint64_t *regvals)
{
	printf("vm_get_register_set\n");
	return -1;
}

static void
arm64_gen_inst_emul_data(struct vcpu *vcpu, uint32_t esr_iss,
    struct vm_exit *vme_ret)
{
	struct vm_guest_paging *paging;
	struct vie *vie;
	uint32_t esr_sas, reg_num;
	uint64_t ttbr0_el1, ttbr1_el1, tcr_el1, tcr_el2, spsr_el1, sctlr_el1;

	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TTBR0_EL1, &ttbr0_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TTBR1_EL1, &ttbr1_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TCR_EL1, &tcr_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TCR_EL2, &tcr_el2);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_SPSR_EL1, &spsr_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr_el1);

	/*
	 * Get the page address from HPFAR_EL2.
	 */
	vme_ret->u.inst_emul.gpa = vcpu->vcpu_exit->exception.physical_address;

	esr_sas = (esr_iss & ISS_DATA_SAS_MASK) >> ISS_DATA_SAS_SHIFT;
	reg_num = (esr_iss & ISS_DATA_SRT_MASK) >> ISS_DATA_SRT_SHIFT;

	vie = &vme_ret->u.inst_emul.vie;
	vie->access_size = 1 << esr_sas;
	vie->sign_extend = (esr_iss & ISS_DATA_SSE) ? 1 : 0;
	vie->dir = (esr_iss & ISS_DATA_WnR) ? VM_DIR_WRITE : VM_DIR_READ;
	vie->reg = reg_num;

	assert(!(esr_iss & ISS_DATA_CM));

	paging = &vme_ret->u.inst_emul.paging;
	paging->ttbr0_addr = ttbr0_el1 & ~(TTBR_ASID_MASK | TTBR_CnP);
	paging->ttbr1_addr = ttbr1_el1 & ~(TTBR_ASID_MASK | TTBR_CnP);
	paging->tcr_el1 = tcr_el1;
	paging->tcr2_el1 = 0;
	paging->flags = spsr_el1 & (PSR_M_MASK | PSR_M_32);
	if ((sctlr_el1 & SCTLR_M) != 0)
		paging->flags |= VM_GP_MMU_ENABLED;
}

static int
sysreg2hvf(int sysreg)
{
	if ((sysreg & (~MRS_CRm_MASK)) ==
	    (MRS_REG(DBGBVR_EL1) & ~MRS_CRm_MASK)) {
		return HV_SYS_REG_DBGBVR0_EL1 +
		    ((sysreg & MRS_CRm_MASK) >> MRS_CRm_SHIFT);
	}
	if ((sysreg & (~MRS_CRm_MASK)) ==
	    (MRS_REG(DBGBCR_EL1) & ~MRS_CRm_MASK)) {
		return HV_SYS_REG_DBGBCR0_EL1 +
		    ((sysreg & MRS_CRm_MASK) >> MRS_CRm_SHIFT);
	}
	if ((sysreg & (~MRS_CRm_MASK)) ==
	    (MRS_REG(DBGWVR_EL1) & ~MRS_CRm_MASK)) {
		return HV_SYS_REG_DBGWVR0_EL1 +
		    ((sysreg & MRS_CRm_MASK) >> MRS_CRm_SHIFT);
	}
	if ((sysreg & (~MRS_CRm_MASK)) ==
	    (MRS_REG(DBGWCR_EL1) & ~MRS_CRm_MASK)) {
		return HV_SYS_REG_DBGWCR0_EL1 +
		    ((sysreg & MRS_CRm_MASK) >> MRS_CRm_SHIFT);
	}

	switch (sysreg) {
	case MDCCINT_EL1:
		return HV_SYS_REG_MDCCINT_EL1;
	case MDSCR_EL1:
		return HV_SYS_REG_MDSCR_EL1;
	case MIDR_EL1:
		return HV_SYS_REG_MIDR_EL1;
	case MPIDR_EL1:
		return HV_SYS_REG_MPIDR_EL1;
	case ID_AA64PFR0_EL1:
		return HV_SYS_REG_ID_AA64PFR0_EL1;
	case ID_AA64PFR1_EL1:
		return HV_SYS_REG_ID_AA64PFR1_EL1;
	case ID_AA64DFR0_EL1:
		return HV_SYS_REG_ID_AA64DFR0_EL1;
	case ID_AA64DFR1_EL1:
		return HV_SYS_REG_ID_AA64DFR1_EL1;
	case ID_AA64ISAR0_EL1:
		return HV_SYS_REG_ID_AA64ISAR0_EL1;
	case ID_AA64ISAR1_EL1:
		return HV_SYS_REG_ID_AA64ISAR1_EL1;
	case ID_AA64MMFR0_EL1:
		return HV_SYS_REG_ID_AA64MMFR0_EL1;
	case ID_AA64MMFR1_EL1:
		return HV_SYS_REG_ID_AA64MMFR1_EL1;
	case ID_AA64MMFR2_EL1:
		return HV_SYS_REG_ID_AA64MMFR2_EL1;
	case MRS_REG(SCTLR_EL1):
		return HV_SYS_REG_SCTLR_EL1;
	// case ACTLR_EL1_REG:
	//	return HV_SYS_REG_ACTLR_EL1;
	case MRS_REG(CPACR_EL1):
		return HV_SYS_REG_CPACR_EL1;
	case MRS_REG(TTBR0_EL1):
		return HV_SYS_REG_TTBR0_EL1;
	case MRS_REG(TTBR1_EL1):
		return HV_SYS_REG_TTBR1_EL1;
	case MRS_REG(TCR_EL1):
		return HV_SYS_REG_TCR_EL1;
	case MRS_REG(APIAKeyLo_EL1):
		return HV_SYS_REG_APIAKEYLO_EL1;
	case MRS_REG(APIAKeyHi_EL1):
		return HV_SYS_REG_APIAKEYHI_EL1;
	case MRS_REG(APIBKeyLo_EL1):
		return HV_SYS_REG_APIBKEYLO_EL1;
	case MRS_REG(APIBKeyHi_EL1):
		return HV_SYS_REG_APIBKEYHI_EL1;
	case MRS_REG(APDAKeyLo_EL1):
		return HV_SYS_REG_APDAKEYLO_EL1;
	case MRS_REG(APDAKeyHi_EL1):
		return HV_SYS_REG_APDAKEYHI_EL1;
	case MRS_REG(APDBKeyLo_EL1):
		return HV_SYS_REG_APDBKEYLO_EL1;
	case MRS_REG(APDBKeyHi_EL1):
		return HV_SYS_REG_APDBKEYHI_EL1;
	case MRS_REG(APGAKeyLo_EL1):
		return HV_SYS_REG_APGAKEYLO_EL1;
	case MRS_REG(APGAKeyHi_EL1):
		return HV_SYS_REG_APGAKEYHI_EL1;
	case MRS_REG(SPSR_EL1):
		return HV_SYS_REG_SPSR_EL1;
	case MRS_REG(ELR_EL1):
		return HV_SYS_REG_ELR_EL1;
	case MRS_REG(SP_EL0):
		return HV_SYS_REG_SP_EL0;
	case MRS_REG(AFSR0_EL1):
		return HV_SYS_REG_AFSR0_EL1;
	case MRS_REG(AFSR1_EL1):
		return HV_SYS_REG_AFSR1_EL1;
	case MRS_REG(ESR_EL1):
		return HV_SYS_REG_ESR_EL1;
	case MRS_REG(FAR_EL1):
		return HV_SYS_REG_FAR_EL1;
	case MRS_REG(PAR_EL1):
		return HV_SYS_REG_PAR_EL1;
	case MRS_REG(MAIR_EL1):
		return HV_SYS_REG_MAIR_EL1;
	case MRS_REG(AMAIR_EL1):
		return HV_SYS_REG_AMAIR_EL1;
	case MRS_REG(VBAR_EL1):
		return HV_SYS_REG_VBAR_EL1;
	case MRS_REG(CONTEXTIDR_EL1):
		return HV_SYS_REG_CONTEXTIDR_EL1;
	case MRS_REG(TPIDR_EL1):
		return HV_SYS_REG_TPIDR_EL1;
	case MRS_REG(CNTKCTL_EL1):
		return HV_SYS_REG_CNTKCTL_EL1;
	case MRS_REG(CSSELR_EL1):
		return HV_SYS_REG_CSSELR_EL1;
	case MRS_REG(CNTV_CTL_EL0):
		return HV_SYS_REG_CNTV_CTL_EL0;
	case MRS_REG(CNTV_CVAL_EL0):
		return HV_SYS_REG_CNTV_CVAL_EL0;
	case VM_REG_GUEST_SP:
		return HV_SYS_REG_SP_EL1;
	}
	return -1;
}

static int
pmu_reg_index(int sysreg, int base_sysreg, int crm_base)
{
	int crm;

	if ((sysreg & ~(MRS_CRm_MASK | MRS_Op2_MASK)) !=
	    (base_sysreg & ~(MRS_CRm_MASK | MRS_Op2_MASK)))
		return (-1);

	crm = (sysreg & MRS_CRm_MASK) >> MRS_CRm_SHIFT;
	if (crm < crm_base || crm > crm_base + 3)
		return (-1);

	return ((crm - crm_base) << 3) |
	    ((sysreg & MRS_Op2_MASK) >> MRS_Op2_SHIFT);
}

static bool
debug_read(struct vcpu *vcpu, int sysreg, uint64_t *val)
{
	switch (sysreg) {
	case OSDLR_EL1:
		*val = vcpu->osdlr_el1;
		return (true);
	case OSLSR_EL1:
		*val = vcpu->oslar_el1 ? (1 << 1) : 0;
		return (true);
	default:
		return (false);
	}
}

static bool
debug_write(struct vcpu *vcpu, int sysreg, uint64_t val)
{
	switch (sysreg) {
	case OSDLR_EL1:
		vcpu->osdlr_el1 = val & 1;
		return (true);
	case OSLAR_EL1:
		vcpu->oslar_el1 = val & 1;
		return (true);
	default:
		return (false);
	}
}

static bool
pmu_read(struct vcpu *vcpu, int sysreg, uint64_t *val)
{
	uint64_t pmcr_fixed;

	pmcr_fixed = ((uint64_t)PMCR_IMP_ARM << PMCR_IMP_SHIFT) |
	    ((uint64_t)PMCR_IDCODE_CORTEX_A57 << PMCR_IDCODE_SHIFT);

	switch (sysreg) {
	case PMCR_EL0:
		*val = pmcr_fixed |
		    (vcpu->pmcr_el0 & (PMCR_E | PMCR_D | PMCR_X | PMCR_DP |
		    PMCR_LC));
		return (true);
	case PMCCNTR_EL0:
	case PMCEID0_EL0:
	case PMCEID1_EL0:
	case PMMIR_EL1:
	case PMXEVCNTR_EL0:
		*val = 0;
		return (true);
	case PMCNTENCLR_EL0:
	case PMCNTENSET_EL0:
		*val = vcpu->pmcntenset_el0;
		return (true);
	case PMINTENCLR_EL1:
	case PMINTENSET_EL1:
		*val = vcpu->pmintenset_el1;
		return (true);
	case PMOVSCLR_EL0:
	case PMOVSSET_EL0:
		*val = vcpu->pmovsclr_el0;
		return (true);
	case PMSELR_EL0:
		*val = vcpu->pmselr_el0;
		return (true);
	case PMUSERENR_EL0:
		*val = vcpu->pmuserenr_el0;
		return (true);
	case PMCCFILTR_EL0:
		*val = vcpu->pmccfiltr_el0;
		return (true);
	case PMXEVTYPER_EL0:
		*val = vcpu->pmxevtyper_el0;
		return (true);
	default:
		if (pmu_reg_index(sysreg, MRS_REG(PMEVCNTR_EL0),
			PMEVCNTR_EL0_CRm) >= 0 ||
		    pmu_reg_index(sysreg, MRS_REG(PMEVTYPER_EL0),
			PMEVTYPER_EL0_CRm) >= 0) {
			*val = 0;
			return (true);
		}
		return (false);
	}
}

static bool
pmu_write(struct vcpu *vcpu, int sysreg, uint64_t val)
{
	switch (sysreg) {
	case PMCR_EL0:
		vcpu->pmcr_el0 = val & (PMCR_E | PMCR_D | PMCR_X | PMCR_DP |
		    PMCR_LC);
		return (true);
	case PMCCNTR_EL0:
	case PMSWINC_EL0:
	case PMXEVCNTR_EL0:
		return (true);
	case PMCNTENSET_EL0:
		vcpu->pmcntenset_el0 |= val & (1ULL << 31);
		return (true);
	case PMCNTENCLR_EL0:
		vcpu->pmcntenset_el0 &= ~val;
		return (true);
	case PMINTENSET_EL1:
		vcpu->pmintenset_el1 |= val & (1ULL << 31);
		return (true);
	case PMINTENCLR_EL1:
		vcpu->pmintenset_el1 &= ~val;
		return (true);
	case PMOVSCLR_EL0:
		vcpu->pmovsclr_el0 &= ~val;
		return (true);
	case PMOVSSET_EL0:
		vcpu->pmovsclr_el0 |= val & (1ULL << 31);
		return (true);
	case PMSELR_EL0:
		vcpu->pmselr_el0 = val & PMSELR_SEL_MASK;
		return (true);
	case PMUSERENR_EL0:
		vcpu->pmuserenr_el0 = val & 0xf;
		return (true);
	case PMCCFILTR_EL0:
		vcpu->pmccfiltr_el0 = val;
		return (true);
	case PMXEVTYPER_EL0:
		vcpu->pmxevtyper_el0 = val;
		return (true);
	default:
		return (pmu_reg_index(sysreg, MRS_REG(PMEVCNTR_EL0),
		    PMEVCNTR_EL0_CRm) >= 0 ||
		    pmu_reg_index(sysreg, MRS_REG(PMEVTYPER_EL0),
		    PMEVTYPER_EL0_CRm) >= 0);
	}
}

static int
arm64_gen_reg_emul_data(struct vcpu *vcpu, uint32_t esr_iss,
    struct vm_exit *vme_ret)
{
	uint32_t reg_num;
	struct vre *vre;
	uint64_t val;
	uint64_t sysreg;
	int hvreg;
	hv_return_t hvret;

	vre = &vme_ret->u.reg_emul.vre;
	vre->inst_syndrome = esr_iss;
	/* ARMv8 Architecture Manual, p. D7-2273: 1 means read */
	vre->dir = (esr_iss & ISS_MSR_DIR) ? VM_DIR_READ : VM_DIR_WRITE;
	reg_num = ISS_MSR_Rt(esr_iss);
	vre->reg = reg_num;

	sysreg = ISS_MSR_SYSREG(esr_iss);
	hvreg = sysreg2hvf(sysreg);
	DPRINTLN("sysreg %#llx, hvreg %#x, reg %x, dir %s", sysreg, hvreg,
	    reg_num, vre->dir == VM_DIR_READ ? "read" : "write");

	if (vre->dir == VM_DIR_READ) {
		if (hvreg >= 0) {
			hvret = hv_vcpu_get_sys_reg(vcpu->vcpu,
			    (hv_sys_reg_t)hvreg, &val);
			if (hvret != HV_SUCCESS) {
				EPRINTLN("hv_vcpu_get_sys_reg(%#x) failed: %x",
				    hvreg, hvret);
				return (1);
			}
		} else if (!debug_read(vcpu, sysreg, &val) &&
		    !pmu_read(vcpu, sysreg, &val)) {
			EPRINTLN("Unhandled MRS sysreg: op0 %u op1 %u CRn %u "
			    "CRm %u op2 %u Rt %u syndrome %#x",
			    ISS_MSR_OP0(esr_iss), ISS_MSR_OP1(esr_iss),
			    ISS_MSR_CRn(esr_iss), ISS_MSR_CRm(esr_iss),
			    ISS_MSR_OP2(esr_iss), reg_num, esr_iss);
			return (1);
		}
		if (reg_num != 31) {
			hvret = hv_vcpu_set_reg(vcpu->vcpu, HV_REG_X0 + reg_num,
			    val);
			if (hvret != HV_SUCCESS) {
				EPRINTLN("hv_vcpu_set_reg(X%u) failed: %x",
				    reg_num, hvret);
				return (1);
			}
		}
	} else {
		if (reg_num == 31) {
			val = 0;
		} else {
			hvret = hv_vcpu_get_reg(vcpu->vcpu, HV_REG_X0 + reg_num,
			    &val);
			if (hvret != HV_SUCCESS) {
				EPRINTLN("hv_vcpu_get_reg(X%u) failed: %x",
				    reg_num, hvret);
				return (1);
			}
		}
		if (hvreg >= 0) {
			hvret = hv_vcpu_set_sys_reg(vcpu->vcpu,
			    (hv_sys_reg_t)hvreg, val);
			if (hvret != HV_SUCCESS) {
				EPRINTLN("hv_vcpu_set_sys_reg(%#x) failed: %x",
				    hvreg, hvret);
				return (1);
			}
		} else if (!debug_write(vcpu, sysreg, val) &&
		    !pmu_write(vcpu, sysreg, val)) {
			EPRINTLN("Unhandled MSR sysreg: op0 %u op1 %u CRn %u "
			    "CRm %u op2 %u Rt %u syndrome %#x value %#llx",
			    ISS_MSR_OP0(esr_iss), ISS_MSR_OP1(esr_iss),
			    ISS_MSR_CRn(esr_iss), ISS_MSR_CRm(esr_iss),
			    ISS_MSR_OP2(esr_iss), reg_num, esr_iss, val);
			return (1);
		}
	}
	return 0;
}

int
vm_inject_exception(struct vcpu *vcpu, uint64_t esr, uint64_t far)
{
	uint64_t pc;
	uint64_t spsr_el1;

	// hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_SPSR_EL1, &spsr_el1);

	// save pc to ELR_EL1
	hv_vcpu_get_reg(vcpu->vcpu, HV_REG_PC, &pc);
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_ELR_EL1, pc);

	// save PSTATE into SPSR_EL1
	hv_vcpu_get_reg(vcpu->vcpu, HV_REG_CPSR, &spsr_el1);

	uint64_t mode = spsr_el1 & (PSR_M_MASK | PSR_M_32);
	if (mode == PSR_M_EL1t) {
		hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_VBAR_EL1, &pc);
	} else if (mode == PSR_M_EL1h) {
		hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_VBAR_EL1, &pc);
		pc += 0x200;
	} else if ((mode & PSR_M_32) == PSR_M_64) {
		hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_VBAR_EL1, &pc);
		/* 64-bit EL0 */
		pc += 0x400;
	} else {
		hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_VBAR_EL1, &pc);
		pc += 0x600;
	}
	hv_vcpu_set_reg(vcpu->vcpu, HV_REG_PC, pc);

	/* Set the new cpsr */
	spsr_el1 &= PSR_FLAGS;
	spsr_el1 |= PSR_DAIF | PSR_M_EL1h;

	/*
	 * Update fields that may change on exeption entry
	 * based on how sctlr_el1 is configured.
	 */
	uint64_t sctlr_el1;
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr_el1);
	if ((sctlr_el1 & SCTLR_SPAN) == 0)
		spsr_el1 |= PSR_PAN;
	if ((sctlr_el1 & SCTLR_DSSBS) == 0)
		sctlr_el1 &= ~PSR_SSBS;
	else
		sctlr_el1 |= PSR_SSBS;
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_SPSR_EL1, spsr_el1);
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_SCTLR_EL1, sctlr_el1);

	// syndrome
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_ESR_EL1, esr);
	hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_FAR_EL1, far);
	return 0;
}

static int
vmexit_handle_exception(struct vcpu *vcpu, struct vm_exit *vme)
{
	uint64_t syndrome;
	uint8_t ec;
	uint32_t esr_iss;
	uint64_t pc, x0;
	uint64_t esr;
	int ret = -1;
	int forward_pc = 0;

	hv_vcpu_get_reg(vcpu->vcpu, HV_REG_PC, &pc);
	vme->pc = pc;
	syndrome = vcpu->vcpu_exit->exception.syndrome;
	ec = ESR_ELx_EXCEPTION(syndrome);
	esr_iss = syndrome & ESR_ELx_ISS_MASK;
	switch (ec) {
	case EXCP_DATA_ABORT_L:
		if (!vm_mem_allocated(vcpu->ctx,
			vcpu->vcpu_exit->exception.physical_address)) {
			arm64_gen_inst_emul_data(vcpu, esr_iss, vme);
			vme->exitcode = VM_EXITCODE_INST_EMUL;
			ret = 1;
		} else {
			EPRINTLN("Memory allocated %llx\n",
			    vcpu->vcpu_exit->exception.physical_address);
		}
		forward_pc = true;
		break;
	case EXCP_MSR:
		// vmm_stat_incr(vcpu, VMEXIT_MSR, 1);
		ret = arm64_gen_reg_emul_data(vcpu, esr_iss, vme);
		vme->exitcode = VM_EXITCODE_REG_EMUL;
#if 0
			uint64_t paddr;
			if (guest_vaddr2paddr(vcpu, pc,  &paddr) ==1) {
				uint8_t *ppc = vm_map_gpa(vcpu->ctx, paddr, 4);
				printf("instr: %x, %x, %x, %x\n", ppc[0], ppc[1], ppc[2], ppc[3]);
			}
#endif
		forward_pc = true;
		break;
	case EXCP_HVC:
		hv_vcpu_get_reg(vcpu->vcpu, HV_REG_X0, &x0);
		DPRINTLN("%llx: VM made an HVC call! x0 register holds 0x%llx",
		    pc, x0);
		vme->exitcode = VM_EXITCODE_SMCCC;
		vme->u.smccc_call.func_id = x0;
		for (int i = 0; i < nitems(vme->u.smccc_call.args); i++)
			hv_vcpu_get_reg(vcpu->vcpu, HV_REG_X0 + i + 1,
			    &vme->u.smccc_call.args[i]);
		ret = 1;
		break;
	case EXCP_BRK:
		EPRINTLN("brkp");
		ret = 0;
		esr = (EXCP_BRK << ESR_ELx_EC_SHIFT);
		vm_inject_exception(vcpu, esr, 0);
		break;
	default:
		EPRINTLN(
		    "cpu %d: Unexpected VM exception: 0x%llx, PC %llx, EC 0x%x, VirtAddr 0x%llx, PhysAddr 0x%llx",
		    vcpu->vcpuid, syndrome, pc, ec,
		    vcpu->vcpu_exit->exception.virtual_address,
		    vcpu->vcpu_exit->exception.physical_address);
		exit(-1);
	}

	if (ret >= 0 && forward_pc) {
		// skip to the next instruction
		hv_vcpu_set_reg(vcpu->vcpu, HV_REG_PC, pc + 4);
	}
	return (ret);
}

__unused static void
dump_regs(struct vcpu *vcpu)
{
	uint64_t regs[] = {
		HV_SYS_REG_MIDR_EL1,
		HV_SYS_REG_MPIDR_EL1,
		HV_SYS_REG_ID_AA64PFR0_EL1,
		HV_SYS_REG_ID_AA64PFR1_EL1,
		HV_SYS_REG_ID_AA64DFR0_EL1,
		HV_SYS_REG_ID_AA64DFR1_EL1,
		HV_SYS_REG_ID_AA64ISAR0_EL1,
		HV_SYS_REG_ID_AA64ISAR1_EL1,
		HV_SYS_REG_ID_AA64MMFR0_EL1,
		HV_SYS_REG_ID_AA64MMFR1_EL1,
		HV_SYS_REG_ID_AA64MMFR2_EL1,
		HV_SYS_REG_SCTLR_EL1,
		HV_SYS_REG_ACTLR_EL1,
		HV_SYS_REG_CPACR_EL1,
		HV_SYS_REG_TTBR0_EL1,
		HV_SYS_REG_TTBR1_EL1,
		HV_SYS_REG_TCR_EL1,
		HV_SYS_REG_SPSR_EL1,
		HV_SYS_REG_ELR_EL1,
		HV_SYS_REG_SP_EL0,
		HV_SYS_REG_AFSR0_EL1,
		HV_SYS_REG_AFSR1_EL1,
		HV_SYS_REG_ESR_EL1,
		HV_SYS_REG_FAR_EL1,
		HV_SYS_REG_PAR_EL1,
		HV_SYS_REG_MAIR_EL1,
		HV_SYS_REG_AMAIR_EL1,
		HV_SYS_REG_VBAR_EL1,
		HV_SYS_REG_CONTEXTIDR_EL1,
		HV_SYS_REG_TPIDR_EL1,
		HV_SYS_REG_CNTKCTL_EL1,
		HV_SYS_REG_CSSELR_EL1,
		HV_SYS_REG_TPIDR_EL0,
		HV_SYS_REG_TPIDRRO_EL0,
		HV_SYS_REG_CNTV_CTL_EL0,
		HV_SYS_REG_CNTV_CVAL_EL0,
		HV_SYS_REG_SP_EL1,
	};
	const char *sregs[] = {
		"HV_SYS_REG_MIDR_EL1",
		"HV_SYS_REG_MPIDR_EL1",
		"HV_SYS_REG_ID_AA64PFR0_EL1",
		"HV_SYS_REG_ID_AA64PFR1_EL1",
		"HV_SYS_REG_ID_AA64DFR0_EL1",
		"HV_SYS_REG_ID_AA64DFR1_EL1",
		"HV_SYS_REG_ID_AA64ISAR0_EL1",
		"HV_SYS_REG_ID_AA64ISAR1_EL1",
		"HV_SYS_REG_ID_AA64MMFR0_EL1",
		"HV_SYS_REG_ID_AA64MMFR1_EL1",
		"HV_SYS_REG_ID_AA64MMFR2_EL1",
		"HV_SYS_REG_SCTLR_EL1",
		"HV_SYS_REG_ACTLR_EL1",
		"HV_SYS_REG_CPACR_EL1",
		"HV_SYS_REG_TTBR0_EL1",
		"HV_SYS_REG_TTBR1_EL1",
		"HV_SYS_REG_TCR_EL1",
		"HV_SYS_REG_SPSR_EL1",
		"HV_SYS_REG_ELR_EL1",
		"HV_SYS_REG_SP_EL0",
		"HV_SYS_REG_AFSR0_EL1",
		"HV_SYS_REG_AFSR1_EL1",
		"HV_SYS_REG_ESR_EL1",
		"HV_SYS_REG_FAR_EL1",
		"HV_SYS_REG_PAR_EL1",
		"HV_SYS_REG_MAIR_EL1",
		"HV_SYS_REG_AMAIR_EL1",
		"HV_SYS_REG_VBAR_EL1",
		"HV_SYS_REG_CONTEXTIDR_EL1",
		"HV_SYS_REG_TPIDR_EL1",
		"HV_SYS_REG_CNTKCTL_EL1",
		"HV_SYS_REG_CSSELR_EL1",
		"HV_SYS_REG_TPIDR_EL0",
		"HV_SYS_REG_TPIDRRO_EL0",
		"HV_SYS_REG_CNTV_CTL_EL0",
		"HV_SYS_REG_CNTV_CVAL_EL0",
		"HV_SYS_REG_SP_EL1",
	};

	uint64_t v;
	for (int i = 0; i < sizeof(regs) / 8; i++) {
		hv_vcpu_get_sys_reg(vcpu->vcpu, regs[i], &v);
		printf("%s %llx\n", sregs[i], v);
	}
	exit(-1);
}

int
vm_run(struct vcpu *vcpu, struct vm_run *vmrun)
{
	hv_return_t res = HV_SUCCESS;
	struct vm_exit *vme = vmrun->vm_exit;
	int i;
	int ret = -1;

	// pre-set registers
	while ((i = BIT_FFS(REGMASK_SIZE, &vcpu->regmask)) != 0) {
		hv_vcpu_set_reg(vcpu->vcpu, HV_REG_X0 + i - 1,
		    vcpu->regs[i - 1]);
		BIT_CLR(REGMASK_SIZE, i - 1, &vcpu->regmask);
	}

	do {
		res = hv_vcpu_run(vcpu->vcpu);
		if (res != HV_SUCCESS) {
			EPRINTLN("hv_vcpu_run() failed %x", res);
			return (-1);
		}

#ifdef VCPU_WATCHDOG
		if (kicked) {
			uint64_t pc, sp, lr;
			hv_vcpu_get_reg(vcpu->vcpu, HV_REG_PC, &pc);
			hv_vcpu_get_reg(vcpu->vcpu, HV_REG_LR, &lr);
			hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_SP_EL0, &sp);
			EPRINTLN("vcpu kicked out: %llx, sp %llx, lr %llx", pc,
			    sp, lr);

			uint64_t paddr;
			if (guest_vaddr2paddr(vcpu, lr, &paddr) == 1) {
				uint8_t *ppc = vm_map_gpa(vcpu->ctx, paddr, 4);
				EPRINTLN(".byte 0x%x, 0x%x, 0x%x, 0x%x", ppc[0],
				    ppc[1], ppc[2], ppc[3]);
			}

			exit(-1);
		}
		atomic_fetch_add(&watchdog_var[vcpu->vcpuid], 1);
#endif

		switch (vcpu->vcpu_exit->reason) {
		case HV_EXIT_REASON_EXCEPTION:
			ret = vmexit_handle_exception(vcpu, vme);
			break;
		case HV_EXIT_REASON_CANCELED:
			DPRINTLN("HV_EXIT_REASON_CANCELED");
			vme->exitcode = VM_EXITCODE_SUSPENDED;
			vme->u.suspended.how = vcpu->ctx->suspend_reason;
			ret = 1;
			break;
		case HV_EXIT_REASON_VTIMER_ACTIVATED:
			EPRINTLN("HV_EXIT_REASON_VTIMER_ACTIVATED");
			break;
		default:
			EPRINTLN("exit reason %d", vcpu->vcpu_exit->reason);
			break;
		}
	} while (!ret);

	if (ret >= 0)
		ret = 0;
	return (ret);
}

int
vm_suspend(struct vmctx *ctx, enum vm_suspend_how how)
{
	int vcpu;
	hv_vcpu_t cpus[MAX_VCPUS] = { 0 };
	int cnt = 0;

	ctx->suspend_reason = how;

	vm_suspend_all_cpus(ctx);
	for (vcpu = 0; vcpu < MAX_VCPUS && ctx->vcpus[vcpu]; vcpu++) {
		cpus[cnt++] = ctx->vcpus[vcpu]->vcpu;
	}
	hv_vcpus_exit(cpus, cnt);

	return (0);
}

int
vm_reinit(struct vmctx *ctx)
{
	printf("vm_reinit\n");
	return -1;
}

int
vm_capability_name2type(const char *capname)
{
	printf("vm_capability_name2type\n");
	return (-1);
}

const char *
vm_capability_type2name(int type)
{
	printf("vm_capability_type2name\n");
	return (NULL);
}

int
vm_get_capability(struct vcpu *vcpu, enum vm_cap_type cap, int *retval)
{
	switch (cap) {
	case VM_CAP_HALT_EXIT:
	case VM_CAP_PAUSE_EXIT:
	case VM_CAP_BRK_EXIT:
	case VM_CAP_SS_EXIT:
	case VM_CAP_MASK_HWINTR:
		*retval = 0;
		return 0;
	case VM_CAP_UNRESTRICTED_GUEST:
		*retval = 1;
		return 0;
	default:
		break;
	}
	return -1;
}

int
vm_set_capability(struct vcpu *vcpu, enum vm_cap_type cap, int val)
{
	printf("vm_set_capability\n");
	return -1;
}

uint64_t *
vm_get_stats(struct vcpu *vcpu, struct timeval *ret_tv, int *ret_entries)
{
	printf("vm_get_stats\n");
	return NULL;
}

const char *
vm_get_stat_desc(struct vmctx *ctx, int index)
{
	printf("vm_get_stat_desc\n");
	return NULL;
}

#ifdef __amd64__
int
vm_get_gpa_pmap(struct vmctx *ctx, uint64_t gpa, uint64_t *pte, int *num)
{
	int error, i;
	struct vm_gpa_pte gpapte;

	bzero(&gpapte, sizeof(gpapte));
	gpapte.gpa = gpa;

	error = ioctl(ctx->fd, VM_GET_GPA_PMAP, &gpapte);

	if (error == 0) {
		*num = gpapte.ptenum;
		for (i = 0; i < gpapte.ptenum; i++)
			pte[i] = gpapte.pte[i];
	}

	return (error);
}

int
vm_gla2gpa(struct vcpu *vcpu, struct vm_guest_paging *paging, uint64_t gla,
    int prot, uint64_t *gpa, int *fault)
{
	struct vm_gla2gpa gg;
	int error;

	bzero(&gg, sizeof(struct vm_gla2gpa));
	gg.prot = prot;
	gg.gla = gla;
	gg.paging = *paging;

	error = vcpu_ioctl(vcpu, VM_GLA2GPA, &gg);
	if (error == 0) {
		*fault = gg.fault;
		*gpa = gg.gpa;
	}
	return (error);
}
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifdef __amd64__
int
vm_copy_setup(struct vcpu *vcpu, struct vm_guest_paging *paging, uint64_t gla,
    size_t len, int prot, struct iovec *iov, int iovcnt, int *fault)
{
	void *va;
	uint64_t gpa, off;
	int error, i, n;

	for (i = 0; i < iovcnt; i++) {
		iov[i].iov_base = 0;
		iov[i].iov_len = 0;
	}

	while (len) {
		assert(iovcnt > 0);
		error = vm_gla2gpa(vcpu, paging, gla, prot, &gpa, fault);
		if (error || *fault)
			return (error);

		off = gpa & PAGE_MASK;
		n = MIN(len, PAGE_SIZE - off);

		va = vm_map_gpa(vcpu->ctx, gpa, n);
		if (va == NULL)
			return (EFAULT);

		iov->iov_base = va;
		iov->iov_len = n;
		iov++;
		iovcnt--;

		gla += n;
		len -= n;
	}
	return (0);
}
#endif

void
vm_copy_teardown(struct iovec *iov __unused, int iovcnt __unused)
{
	/*
	 * Intentionally empty.  This is used by the instruction
	 * emulation code shared with the kernel.  The in-kernel
	 * version of this is non-empty.
	 */
}

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
		bcopy(src, dst, n);

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
		bcopy(src, dst, n);

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
		memcpy(cpus, &ctx->active_cpus, sizeof(cpuset_t));
		return (0);
	case VM_SUSPENDED_CPUS:
		memcpy(cpus, &ctx->suspended_cpus, sizeof(cpuset_t));
		return (0);
	}
	return (-1);
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
	if (CPU_ISSET(((unsigned)vcpu->vcpuid), &vcpu->ctx->active_cpus))
		return (EBUSY);

	CPU_CLR_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->active_cpus);
	CPU_SET_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->active_cpus);

	return (0);
}

int
vm_suspend_all_cpus(struct vmctx *ctx)
{
	int vcpu;

	for (vcpu = 0; vcpu < MAX_VCPUS && ctx->vcpus[vcpu]; vcpu++) {
		vm_suspend_cpu(ctx->vcpus[vcpu]);
	}

	return -1;
}

int
vm_suspend_cpu(struct vcpu *vcpu)
{
	CPU_CLR_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->active_cpus);
	CPU_SET_ATOMIC((unsigned)vcpu->vcpuid, &vcpu->ctx->suspended_cpus);
	return (0);
}

int
vm_resume_cpu(struct vcpu *vcpu)
{
	return vm_activate_cpu(vcpu);
}

int
vm_resume_all_cpus(struct vmctx *ctx)
{
	printf("vm_resume_all_cpus\n");
	return -1;
}

#ifdef __amd64__
int
vm_get_intinfo(struct vcpu *vcpu, uint64_t *info1, uint64_t *info2)
{
	struct vm_intinfo vmii;
	int error;

	bzero(&vmii, sizeof(struct vm_intinfo));
	error = vcpu_ioctl(vcpu, VM_GET_INTINFO, &vmii);
	if (error == 0) {
		*info1 = vmii.info1;
		*info2 = vmii.info2;
	}
	return (error);
}

int
vm_set_intinfo(struct vcpu *vcpu, uint64_t info1)
{
	struct vm_intinfo vmii;
	int error;

	bzero(&vmii, sizeof(struct vm_intinfo));
	vmii.info1 = info1;
	error = vcpu_ioctl(vcpu, VM_SET_INTINFO, &vmii);
	return (error);
}
#endif

#ifdef WITH_VMMAPI_SNAPSHOT
int
vm_restart_instruction(struct vcpu *vcpu)
{
	int arg;

	return (vcpu_ioctl(vcpu, VM_RESTART_INSTRUCTION, &arg));
}

int
vm_snapshot_req(struct vmctx *ctx, struct vm_snapshot_meta *meta)
{
	if (ioctl(ctx->fd, VM_SNAPSHOT_REQ, meta) == -1) {
#ifdef SNAPSHOT_DEBUG
		fprintf(stderr, "%s: snapshot failed for %s: %d\r\n", __func__,
		    meta->dev_name, errno);
#endif
		return (-1);
	}
	return (0);
}

int
vm_restore_time(struct vmctx *ctx)
{
	int dummy;

	dummy = 0;
	return (ioctl(ctx->fd, VM_RESTORE_TIME, &dummy));
}
#endif

int
vm_set_topology(struct vmctx *ctx, uint16_t sockets, uint16_t cores,
    uint16_t threads, uint16_t maxcpus)
{
	return 0;
}

// ChagGPT at its best. I don't understand what this code does, nor I want to
// know
int
vm_get_topology(struct vmctx *ctx, uint16_t *sockets, uint16_t *cores,
    uint16_t *threads, uint16_t *maxcpus)
{
#if defined(__APPLE__)
	/*
	 * macOS approach:
	 *   - "hw.packages"    => # of physical CPU packages (sockets)
	 *   - "hw.physicalcpu" => # of physical cores (currently available to
	 * OS)
	 *   - "hw.logicalcpu"  => # of logical CPUs (currently available to OS)
	 *   - "hw.logicalcpu_max", "hw.physicalcpu_max" => may also exist
	 *
	 * On most Mac systems, #sockets = 1 unless it's a special multi-socket
	 * Mac Pro.
	 */
	{
		size_t len = sizeof(uint32_t);
		uint32_t val;

		// Sockets
		uint32_t packages = 1; // default to 1 if sysctl not present
		if (sysctlbyname("hw.packages", &val, &len, NULL, 0) == 0) {
			packages = val;
		}

		// Physical cores
		uint32_t phys_cores = 1; // fallback
		if (sysctlbyname("hw.physicalcpu", &val, &len, NULL, 0) == 0) {
			phys_cores = val;
		}

		// Logical CPUs
		uint32_t log_cpus = 1; // fallback
		if (sysctlbyname("hw.logicalcpu", &val, &len, NULL, 0) == 0) {
			log_cpus = val;
		}

		// Max CPUs (sometimes "hw.logicalcpu_max" is available;
		// fallback to log_cpus if not)
		uint32_t max_cpus = log_cpus;
		if (sysctlbyname("hw.logicalcpu_max", &val, &len, NULL, 0) ==
		    0) {
			max_cpus = val;
		}

		// Cast or clamp to uint16_t safely
		*sockets = (uint16_t)packages;
		*cores = (uint16_t)phys_cores;
		*threads = (uint16_t)log_cpus;
		*maxcpus = (uint16_t)max_cpus;
	}

	return 0;

#elif defined(__linux__)
	/*
	 * Linux approach (VERY simplified):
	 *   - # of online logical CPUs: sysconf(_SC_NPROCESSORS_ONLN)
	 *   - # of configured CPUs    : sysconf(_SC_NPROCESSORS_CONF)
	 *   - # of sockets, # of physical cores per socket, etc. can be
	 * discovered via:
	 *        - /proc/cpuinfo  (parse "physical id", "core id")
	 *        - /sys/devices/system/cpu/cpuN/topology/*
	 *     For brevity, we'll just do a single-socket guess or read from
	 * /proc/cpuinfo.
	 */
	{
		// 1) Get total number of *online* logical CPUs:
		long online_logical = sysconf(_SC_NPROCESSORS_ONLN);
		if (online_logical < 1) {
			return -1; // error
		}

		// 2) Get total number of *configured* CPUs (maxcpus):
		long configured = sysconf(_SC_NPROCESSORS_CONF);
		if (configured < 1) {
			return -1; // error
		}

		// If you want a more accurate socket/core count, parse
		// /proc/cpuinfo or sysfs. For a naive approach, assume 1 socket
		// and (online_logical) / 2 for cores (assuming
		// hyper-threading): This can be wildly incorrect on
		// multi-socket systems or non-HT. We'll do a quick parse of
		// /proc/cpuinfo to identify unique physical id + core id.

		FILE *fp = fopen("/proc/cpuinfo", "r");
		if (!fp) {
			// Fallback: just do single-socket guess
			*sockets = 1;
			*threads = (uint16_t)online_logical;
			*cores = (uint16_t)
			    online_logical; // guess no hyper-threading
			*maxcpus = (uint16_t)configured;
			return 0;
		}

/* We'll track physical_id -> set of core_ids */
#define MAX_CPU	   1024 // arbitrary
		int physical_id[MAX_CPU];
		int core_id[MAX_CPU];
		memset(physical_id, -1, sizeof(physical_id));
		memset(core_id, -1, sizeof(core_id));

		int cpu_index =
		    0; // index for each "processor" block in /proc/cpuinfo

		char line[256];
		int cur_phys_id = -1, cur_core_id = -1;
		while (fgets(line, sizeof(line), fp)) {
			if (strncmp(line, "processor", 9) == 0) {
				// new CPU block
				// store the previous CPU info if valid
				if (cpu_index < MAX_CPU && cur_phys_id != -1) {
					physical_id[cpu_index] = cur_phys_id;
					core_id[cpu_index] = cur_core_id;
					cpu_index++;
				}
				// reset for next
				cur_phys_id = -1;
				cur_core_id = -1;
			} else if (strncmp(line, "physical id", 11) == 0) {
				sscanf(line, "physical id : %d", &cur_phys_id);
			} else if (strncmp(line, "core id", 7) == 0) {
				sscanf(line, "core id : %d", &cur_core_id);
			}
		}
		// Store the last CPU block if needed
		if (cpu_index < MAX_CPU && cur_phys_id != -1) {
			physical_id[cpu_index] = cur_phys_id;
			core_id[cpu_index] = cur_core_id;
			cpu_index++;
		}
		fclose(fp);

		if (cpu_index == 0) {
			// fallback if parsing didn't work
			*sockets = 1;
			*threads = (uint16_t)online_logical;
			*cores = (uint16_t)online_logical;
			*maxcpus = (uint16_t)configured;
			return 0;
		}

		// Count unique physical_id and (physical_id, core_id) pairs
		int socket_count = 0;
		int core_count = 0;
// We'll store each unique physical_id in an array
#define MAX_SOCKET 64
		int sockets_seen[MAX_SOCKET];
		int sockets_used = 0;
		memset(sockets_seen, -1, sizeof(sockets_seen));

// same for each unique (phys_id, core_id)
#define MAX_CORES  1024
		struct {
			int phys;
			int core;
		} core_map[MAX_CORES];
		int core_map_used = 0;

		for (int i = 0; i < cpu_index; i++) {
			int pid = physical_id[i];
			int cid = core_id[i];

			// check if pid is already in sockets_seen
			int found_socket = 0;
			for (int s = 0; s < sockets_used; s++) {
				if (sockets_seen[s] == pid) {
					found_socket = 1;
					break;
				}
			}
			if (!found_socket && sockets_used < MAX_SOCKET) {
				sockets_seen[sockets_used++] = pid;
			}

			// check if (pid,cid) is already in core_map
			int found_core = 0;
			for (int c = 0; c < core_map_used; c++) {
				if (core_map[c].phys == pid &&
				    core_map[c].core == cid) {
					found_core = 1;
					break;
				}
			}
			if (!found_core && core_map_used < MAX_CORES) {
				core_map[core_map_used].phys = pid;
				core_map[core_map_used].core = cid;
				core_map_used++;
			}
		}

		socket_count = sockets_used;
		core_count = core_map_used; // total unique cores

		*sockets = (uint16_t)socket_count;
		*cores = (uint16_t)core_count;
		*threads = (uint16_t)online_logical;
		*maxcpus = (uint16_t)configured;

		return 0;
	}
#else
	// Fallback for other platforms
	errno = ENOSYS; // function not implemented
	return -1;
#endif
}

int
vm_limit_rights(struct vmctx *ctx)
{
	return (0);
}

/*
 * Avoid using in new code.  Operations on the fd should be wrapped here so that
 * capability rights can be kept in sync.
 */
int
vm_get_device_fd(struct vmctx *ctx)
{
	return 0;
}

/* log2 of the number of bytes in a page table entry */
#define PTE_SHIFT 3
int
vmmops_gla2gpa(struct vcpu *vcpu, struct vm_guest_paging *paging, uint64_t gla,
    int prot, uint64_t *gpa, int *is_fault)
{
	uint64_t mask, *ptep, pte, pte_addr;
	int address_bits, granule_shift, ia_bits, levels, pte_shift, tsz;
	bool is_el0;

	/* Check if the MMU is off */
	if ((paging->flags & VM_GP_MMU_ENABLED) == 0) {
		*is_fault = 0;
		*gpa = gla;
		return (0);
	}

	is_el0 = (paging->flags & PSR_M_MASK) == PSR_M_EL0t;

	if (ADDR_IS_KERNEL(gla)) {
		/* If address translation is disabled raise an exception */
		if ((paging->tcr_el1 & TCR_EPD1) != 0) {
			*is_fault = 1;
			return (0);
		}
		if (is_el0 && (paging->tcr_el1 & TCR_E0PD1) != 0) {
			*is_fault = 1;
			return (0);
		}
		pte_addr = paging->ttbr1_addr;
		tsz = (paging->tcr_el1 & TCR_T1SZ_MASK) >> TCR_T1SZ_SHIFT;
		/* Clear the top byte if TBI is on */
		if ((paging->tcr_el1 & TCR_TBI1) != 0)
			gla |= (0xfful << 56);
		switch (paging->tcr_el1 & TCR_TG1_MASK) {
		case TCR_TG1_4K:
			granule_shift = PAGE_SHIFT_4K;
			break;
		case TCR_TG1_16K:
			granule_shift = PAGE_SHIFT_16K;
			break;
		case TCR_TG1_64K:
			granule_shift = PAGE_SHIFT_64K;
			break;
		default:
			*is_fault = 1;
			return (EINVAL);
		}
	} else {
		/* If address translation is disabled raise an exception */
		if ((paging->tcr_el1 & TCR_EPD0) != 0) {
			*is_fault = 1;
			return (0);
		}
		if (is_el0 && (paging->tcr_el1 & TCR_E0PD0) != 0) {
			*is_fault = 1;
			return (0);
		}
		pte_addr = paging->ttbr0_addr;
		tsz = (paging->tcr_el1 & TCR_T0SZ_MASK) >> TCR_T0SZ_SHIFT;
		/* Clear the top byte if TBI is on */
		if ((paging->tcr_el1 & TCR_TBI0) != 0)
			gla &= ~(0xfful << 56);
		switch (paging->tcr_el1 & TCR_TG0_MASK) {
		case TCR_TG0_4K:
			granule_shift = PAGE_SHIFT_4K;
			break;
		case TCR_TG0_16K:
			granule_shift = PAGE_SHIFT_16K;
			break;
		case TCR_TG0_64K:
			granule_shift = PAGE_SHIFT_64K;
			break;
		default:
			*is_fault = 1;
			return (EINVAL);
		}
	}

	/*
	 * TODO: Support FEAT_TTST for smaller tsz values and FEAT_LPA2
	 * for larger values.
	 */
	switch (granule_shift) {
	case PAGE_SHIFT_4K:
	case PAGE_SHIFT_16K:
		/*
		 * See "Table D8-11 4KB granule, determining stage 1 initial
		 * lookup level" and "Table D8-21 16KB granule, determining
		 * stage 1 initial lookup level" from the "Arm Architecture
		 * Reference Manual for A-Profile architecture" revision I.a
		 * for the minimum and maximum values.
		 *
		 * TODO: Support less than 16 when FEAT_LPA2 is implemented
		 * and TCR_EL1.DS == 1
		 * TODO: Support more than 39 when FEAT_TTST is implemented
		 */
		if (tsz < 16 || tsz > 39) {
			*is_fault = 1;
			return (EINVAL);
		}
		break;
	case PAGE_SHIFT_64K:
	/* TODO: Support 64k granule. It will probably work, but is untested */
	default:
		*is_fault = 1;
		return (EINVAL);
	}

	/*
	 * Calculate the input address bits. These are 64 bit in an address
	 * with the top tsz bits being all 0 or all 1.
	 */
	ia_bits = 64 - tsz;

	/*
	 * Calculate the number of address bits used in the page table
	 * calculation. This is ia_bits minus the bottom granule_shift
	 * bits that are passed to the output address.
	 */
	address_bits = ia_bits - granule_shift;

	/*
	 * Calculate the number of levels. Each level uses
	 * granule_shift - PTE_SHIFT bits of the input address.
	 * This is because the table is 1 << granule_shift and each
	 * entry is 1 << PTE_SHIFT bytes.
	 */
	levels = howmany(address_bits, granule_shift - PTE_SHIFT);

	/* Mask of the upper unused bits in the virtual address */
	gla &= (1ul << ia_bits) - 1;
	/* TODO: Check if the level supports block descriptors */
	for (; levels > 0; levels--) {
		int idx;

		pte_shift = (levels - 1) * (granule_shift - PTE_SHIFT) +
		    granule_shift;
		idx = (gla >> pte_shift) &
		    ((1ul << (granule_shift - PTE_SHIFT)) - 1);
		while (idx > PAGE_SIZE / sizeof(pte)) {
			idx -= PAGE_SIZE / sizeof(pte);
			pte_addr += PAGE_SIZE;
		}

		ptep = vm_map_gpa(vcpu->ctx, pte_addr, PAGE_SIZE);
		if (ptep == NULL)
			goto error;
		pte = ptep[idx];

		/* Calculate the level we are looking at */
		switch (levels) {
		default:
			goto fault;
		/* TODO: Level -1 when FEAT_LPA2 is implemented */
		case 4: /* Level 0 */
			if ((pte & ATTR_DESCR_MASK) != L0_TABLE)
				goto fault;
			/* FALLTHROUGH */
		case 3: /* Level 1 */
		case 2: /* Level 2 */
			switch (pte & ATTR_DESCR_MASK) {
			/* Use L1 macro as all levels are the same */
			case L1_TABLE:
				/* Check if EL0 can access this address space */
				if (is_el0 &&
				    (pte & TATTR_AP_TABLE_NO_EL0) != 0)
					goto fault;
				/* Check if the address space is writable */
				if ((prot & PROT_WRITE) != 0 &&
				    (pte & TATTR_AP_TABLE_RO) != 0)
					goto fault;
				if ((prot & PROT_EXEC) != 0) {
					/* Check the table exec attribute */
					if ((is_el0 &&
						(pte & TATTR_UXN_TABLE) != 0) ||
					    (!is_el0 &&
						(pte & TATTR_PXN_TABLE) != 0))
						goto fault;
				}
				pte_addr = pte & ~ATTR_MASK;
				break;
			case L1_BLOCK:
				goto done;
			default:
				goto fault;
			}
			break;
		case 1: /* Level 3 */
			if ((pte & ATTR_DESCR_MASK) == L3_PAGE)
				goto done;
			goto fault;
		}
	}

done:
	/* Check if EL0 has access to the block/page */
	if (is_el0 && (pte & ATTR_S1_AP(ATTR_S1_AP_USER)) == 0)
		goto fault;
	if ((prot & PROT_WRITE) != 0 && (pte & ATTR_S1_AP_RW_BIT) != 0)
		goto fault;
	if ((prot & PROT_EXEC) != 0) {
		if ((is_el0 && (pte & ATTR_S1_UXN) != 0) ||
		    (!is_el0 && (pte & ATTR_S1_PXN) != 0))
			goto fault;
	}
	mask = (1ul << pte_shift) - 1;
	*gpa = (pte & ~ATTR_MASK) | (gla & mask);
	*is_fault = 0;
	return (0);

error:
	return (EFAULT);
fault:
	*is_fault = 1;
	return (0);
}

static int
guest_paging_info(struct vcpu *vcpu, struct vm_guest_paging *paging)
{
	uint64_t ttbr0_el1, ttbr1_el1, tcr_el1, tcr_el2, spsr_el1, sctlr_el1;

	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TTBR0_EL1, &ttbr0_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TTBR1_EL1, &ttbr1_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TCR_EL1, &tcr_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_TCR_EL2, &tcr_el2);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_SPSR_EL1, &spsr_el1);
	hv_vcpu_get_sys_reg(vcpu->vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr_el1);

	memset(paging, 0, sizeof(*paging));
	paging->ttbr0_addr = ttbr0_el1 & ~(TTBR_ASID_MASK | TTBR_CnP);
	paging->ttbr1_addr = ttbr1_el1 & ~(TTBR_ASID_MASK | TTBR_CnP);
	paging->tcr_el1 = tcr_el1;
	paging->tcr2_el1 = 0;
	paging->flags = spsr_el1 & (PSR_M_MASK | PSR_M_32);
	if ((sctlr_el1 & SCTLR_M) != 0)
		paging->flags |= VM_GP_MMU_ENABLED;

	return (0);
}

/*
 * Map a guest virtual address to a physical address (for a given vcpu).
 * If a guest virtual address is valid, return 1.  If the address is
 * not valid, return 0.  If an error occurs obtaining the mapping,
 * return -1.
 */
__unused static int
guest_vaddr2paddr(struct vcpu *vcpu, uint64_t vaddr, uint64_t *paddr)
{
	struct vm_guest_paging paging;
	int fault;

	if (guest_paging_info(vcpu, &paging) == -1)
		return (-1);

	/*
	 * Always use PROT_READ.  We really care if the VA is
	 * accessible, not if the current vCPU can write.
	 */
	if (vm_gla2gpa_nofault(vcpu, &paging, vaddr, PROT_READ, paddr,
		&fault) == -1)
		return (-1);
	if (fault)
		return (0);
	return (1);
}

int
vm_gla2gpa_nofault(struct vcpu *vcpu, struct vm_guest_paging *paging,
    uint64_t gla, int prot, uint64_t *gpa, int *is_fault)
{
	vmmops_gla2gpa(vcpu, paging, gla, prot, gpa, is_fault);
	return (0);
}
