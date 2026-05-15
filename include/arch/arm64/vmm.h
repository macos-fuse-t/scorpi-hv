/*
 * Copyright (C) 2015 Mihai Carabas <mihai.carabas@gmail.com>
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
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _VMM_H_
#define	_VMM_H_

#include "common.h"
#include <sys/param.h>
#include <support/cpuset.h>
//#include <vm/vm.h>
//#include <vm/pmap.h>

//#include "pte.h"
//#include "pmap.h"

struct vcpu;

enum vm_suspend_how {
	VM_SUSPEND_NONE,
	VM_SUSPEND_RESET,
	VM_SUSPEND_POWEROFF,
	VM_SUSPEND_HALT,
	VM_SUSPEND_LAST
};

/*
 * Identifiers for architecturally defined registers.
 */
enum vm_reg_name {
	VM_REG_GUEST_X0 = 0,
	VM_REG_GUEST_X1,
	VM_REG_GUEST_X2,
	VM_REG_GUEST_X3,
	VM_REG_GUEST_X4,
	VM_REG_GUEST_X5,
	VM_REG_GUEST_X6,
	VM_REG_GUEST_X7,
	VM_REG_GUEST_X8,
	VM_REG_GUEST_X9,
	VM_REG_GUEST_X10,
	VM_REG_GUEST_X11,
	VM_REG_GUEST_X12,
	VM_REG_GUEST_X13,
	VM_REG_GUEST_X14,
	VM_REG_GUEST_X15,
	VM_REG_GUEST_X16,
	VM_REG_GUEST_X17,
	VM_REG_GUEST_X18,
	VM_REG_GUEST_X19,
	VM_REG_GUEST_X20,
	VM_REG_GUEST_X21,
	VM_REG_GUEST_X22,
	VM_REG_GUEST_X23,
	VM_REG_GUEST_X24,
	VM_REG_GUEST_X25,
	VM_REG_GUEST_X26,
	VM_REG_GUEST_X27,
	VM_REG_GUEST_X28,
	VM_REG_GUEST_X29,
	VM_REG_GUEST_LR,
	VM_REG_GUEST_SP,
	VM_REG_GUEST_PC,
	VM_REG_GUEST_CPSR,

	VM_REG_GUEST_SCTLR_EL1,
	VM_REG_GUEST_TTBR0_EL1,
	VM_REG_GUEST_TTBR1_EL1,
	VM_REG_GUEST_TCR_EL1,
	VM_REG_GUEST_TCR2_EL1,
	VM_REG_LAST
};

#define	VM_INTINFO_VECTOR(info)	((info) & 0xff)
#define	VM_INTINFO_DEL_ERRCODE	0x800
#define	VM_INTINFO_RSVD		0x7ffff000
#define	VM_INTINFO_VALID	0x80000000
#define	VM_INTINFO_TYPE		0x700
#define	VM_INTINFO_HWINTR	(0 << 8)
#define	VM_INTINFO_NMI		(2 << 8)
#define	VM_INTINFO_HWEXCEPTION	(3 << 8)
#define	VM_INTINFO_SWINTR	(4 << 8)

#define VM_GUEST_BASE_IPA	0x80000000UL	/* Guest kernel start ipa */
#define	VM_FDT_BASE		0xFFFF0000UL

/*
 * The VM name has to fit into the pathname length constraints of devfs,
 * governed primarily by SPECNAMELEN.  The length is the total number of
 * characters in the full path, relative to the mount point and not 
 * including any leading '/' characters.
 * A prefix and a suffix are added to the name specified by the user.
 * The prefix is usually "vmm/" or "vmm.io/", but can be a few characters
 * longer for future use.
 * The suffix is a string that identifies a bootrom image or some similar
 * image that is attached to the VM. A separator character gets added to
 * the suffix automatically when generating the full path, so it must be
 * accounted for, reducing the effective length by 1.
 * The effective length of a VM name is 229 bytes for FreeBSD 13 and 37
 * bytes for FreeBSD 12.  A minimum length is set for safety and supports
 * a SPECNAMELEN as small as 32 on old systems.
 */
#define VM_MAX_PREFIXLEN 10
#define VM_MAX_SUFFIXLEN 15
#define VM_MAX_NAMELEN 	128

#define	VM_DIR_READ	0
#define	VM_DIR_WRITE	1

#define	VM_GP_M_MASK		0x1f
#define	VM_GP_MMU_ENABLED	(1 << 5)
#define	VM_GP_EL2		(1 << 6)

struct vm_guest_paging {
	uint64_t	ttbr0_addr;
	uint64_t	ttbr1_addr;
	uint64_t	tcr_el1;
	uint64_t	tcr2_el1;
	int		flags;
	int		padding;
};

struct vie {
	uint8_t access_size:4, sign_extend:1, dir:1, unused:2;
	enum vm_reg_name reg;
};

struct vre {
	uint32_t inst_syndrome;
	uint8_t dir:1, unused:7;
	enum vm_reg_name reg;
};

/*
 * Identifiers for optional vmm capabilities
 */
enum vm_cap_type {
	VM_CAP_HALT_EXIT,
	VM_CAP_PAUSE_EXIT,
	VM_CAP_UNRESTRICTED_GUEST,
	VM_CAP_BRK_EXIT,
	VM_CAP_SS_EXIT,
	VM_CAP_MASK_HWINTR,
	VM_CAP_MAX
};

enum vm_exitcode {
	VM_EXITCODE_BOGUS,
	VM_EXITCODE_INST_EMUL,
	VM_EXITCODE_REG_EMUL,
	VM_EXITCODE_HVC,
	VM_EXITCODE_SUSPENDED,
	VM_EXITCODE_HYP,
	VM_EXITCODE_WFI,
	VM_EXITCODE_PAGING,
	VM_EXITCODE_SMCCC,
	VM_EXITCODE_DEBUG,
	VM_EXITCODE_BRK,
	VM_EXITCODE_SS,
	VM_EXITCODE_MAX
};

struct vm_exit {
	enum vm_exitcode	exitcode;
	int			inst_length;
	uint64_t		pc;
	union {
		/*
		 * ARM specific payload.
		 */
		struct {
			uint32_t	exception_nr;
			uint32_t	pad;
			uint64_t	esr_el2;	/* Exception Syndrome Register */
			uint64_t	far_el2;	/* Fault Address Register */
			uint64_t	hpfar_el2;	/* Hypervisor IPA Fault Address Register */
		} hyp;
		struct {
			struct vre 	vre;
		} reg_emul;
		struct {
			uint64_t	gpa;
			uint64_t	esr;
		} paging;
		struct {
			uint64_t	gpa;
			struct vm_guest_paging paging;
			struct vie	vie;
		} inst_emul;

		/*
		 * A SMCCC call, e.g. starting a core via PSCI.
		 * Further arguments can be read by asking the kernel for
		 * all register values.
		 */
		struct {
			uint64_t	func_id;
			uint64_t	args[7];
		} smccc_call;

		struct {
			enum vm_suspend_how how;
		} suspended;
	} u;
};

#endif	/* _VMM_H_ */
