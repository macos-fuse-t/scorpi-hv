/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <vmmapi.h>

#include "arch/x86/inout.h"
#include "bhyverun.h"
#include "bootrom.h"
#include "debug.h"
#include "mem.h"
#include "vmexit.h"

static uint64_t
vmexit_reg(struct vcpu *vcpu, int reg)
{
	uint64_t val;

	if (vm_get_register(vcpu, reg, &val) != 0)
		return (0);

	return (val);
}

static void
vmexit_dump_regs(struct vcpu *vcpu)
{

	EPRINTLN("vcpu %d state: rip %#llx rsp %#llx rflags %#llx",
	    vcpu_id(vcpu),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RIP),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RSP),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RFLAGS));
	EPRINTLN("vcpu %d state: cr0 %#llx cr2 %#llx cr3 %#llx cr4 %#llx",
	    vcpu_id(vcpu),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_CR0),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_CR2),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_CR3),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_CR4));
	EPRINTLN("vcpu %d state: efer %#llx rax %#llx rbx %#llx rcx %#llx",
	    vcpu_id(vcpu),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_EFER),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RAX),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RBX),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RCX));
	EPRINTLN("vcpu %d state: rdx %#llx rsi %#llx rdi %#llx rbp %#llx",
	    vcpu_id(vcpu),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RDX),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RSI),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RDI),
	    (unsigned long long)vmexit_reg(vcpu, VM_REG_GUEST_RBP));
}

static int
vmexit_inout(struct vmctx *ctx, struct vcpu *vcpu, struct vm_run *vmrun)
{
	struct vm_exit *vme;
	int error;

	vme = vmrun->vm_exit;
	error = emulate_inout(ctx, vcpu, vme);
	if (error != 0) {
		EPRINTLN("Unhandled %s access to port %#x at rip %#llx",
		    vme->u.inout.in ? "in" : "out", vme->u.inout.port,
		    (unsigned long long)vme->rip);
		return (VMEXIT_ABORT);
	}

	if (vme->u.inout.in) {
		error = vm_set_register(vcpu, VM_REG_GUEST_RAX,
		    vme->u.inout.eax);
		if (error != 0)
			return (VMEXIT_ABORT);
	}

	return (VMEXIT_CONTINUE);
}

static int
vmexit_inout_str(struct vmctx *ctx __unused, struct vcpu *vcpu __unused,
    struct vm_run *vmrun)
{
	struct vm_exit *vme;

	vme = vmrun->vm_exit;
	EPRINTLN("Unhandled string I/O access to port %#x at rip %#llx",
	    vme->u.inout_str.inout.port, (unsigned long long)vme->rip);
	return (VMEXIT_ABORT);
}

static int
vmexit_inst_emul(struct vmctx *ctx __unused, struct vcpu *vcpu,
    struct vm_run *vmrun)
{
	struct vm_exit *vme;
	int error;

	vme = vmrun->vm_exit;
	error = emulate_mem(vcpu, vme->u.inst_emul.gpa,
	    &vme->u.inst_emul.vie, &vme->u.inst_emul.paging);
	if (error != 0) {
		if (error == ESRCH) {
			EPRINTLN("Unhandled memory access to %#llx",
			    (unsigned long long)vme->u.inst_emul.gpa);
		}
		EPRINTLN("Failed to emulate instruction at rip %#llx",
		    (unsigned long long)vme->rip);
		return (VMEXIT_ABORT);
	}

	return (VMEXIT_CONTINUE);
}

static int
vmexit_suspend(struct vmctx *ctx, struct vcpu *vcpu, struct vm_run *vmrun)
{
	struct vm_exit *vme;

	vme = vmrun->vm_exit;
	switch (vme->u.suspended.how) {
	case VM_SUSPEND_RESET:
		if (vcpu_id(vcpu) == 0) {
			vm_activate_cpu(vcpu);
			if (vcpu_reset(vcpu) != 0)
				return (VMEXIT_ABORT);
			return (VMEXIT_CONTINUE);
		}
		return (VMEXIT_QUIT);
	case VM_SUSPEND_POWEROFF:
		vm_destroy(ctx);
		exit(0);
	case VM_SUSPEND_HALT:
		exit(2);
	case VM_SUSPEND_TRIPLEFAULT:
		EPRINTLN("vcpu %d triple fault", vcpu_id(vcpu));
		vmexit_dump_regs(vcpu);
		exit(4);
	default:
		EPRINTLN("vmexit_suspend: invalid reason %d",
		    vme->u.suspended.how);
		return (VMEXIT_ABORT);
	}
}

static int
vmexit_bogus(struct vmctx *ctx __unused, struct vcpu *vcpu __unused,
    struct vm_run *vmrun __unused)
{
	return (VMEXIT_CONTINUE);
}

static int
vmexit_hlt(struct vmctx *ctx __unused, struct vcpu *vcpu __unused,
    struct vm_run *vmrun __unused)
{
	return (VMEXIT_CONTINUE);
}

static int
vmexit_abort(struct vmctx *ctx __unused, struct vcpu *vcpu __unused,
    struct vm_run *vmrun)
{
	struct vm_exit *vme;

	vme = vmrun->vm_exit;
	EPRINTLN("Unhandled x86 vmexit %d at rip %#llx", vme->exitcode,
	    (unsigned long long)vme->rip);
	return (VMEXIT_ABORT);
}

const vmexit_handler_t vmexit_handlers[VM_EXITCODE_MAX] = {
	[VM_EXITCODE_INOUT] = vmexit_inout,
	[VM_EXITCODE_BOGUS] = vmexit_bogus,
	[VM_EXITCODE_RDMSR] = vmexit_abort,
	[VM_EXITCODE_WRMSR] = vmexit_abort,
	[VM_EXITCODE_HLT] = vmexit_hlt,
	[VM_EXITCODE_PAUSE] = vmexit_bogus,
	[VM_EXITCODE_INST_EMUL] = vmexit_inst_emul,
	[VM_EXITCODE_IOAPIC_EOI] = vmexit_bogus,
	[VM_EXITCODE_SUSPENDED] = vmexit_suspend,
	[VM_EXITCODE_INOUT_STR] = vmexit_inout_str,
	[VM_EXITCODE_IPI] = vmexit_bogus,
};
