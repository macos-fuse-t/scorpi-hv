/* Linux/KVM bring-up smoke test for the ARM64 backend. */

#include <errno.h>
#include <sys/mman.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vmmapi.h>

#define	TEST_GPA	0x40000000ULL
#define	TEST_GIC_DIST_BASE	0x2f000000ULL
#define	TEST_GIC_DIST_SIZE	0x10000UL
#define	TEST_GIC_REDIST_BASE	0x2f100000ULL
#define	TEST_GIC_REDIST_SIZE	0x20000UL
#define	TEST_MEM_SIZE	0x10000UL
#define	TEST_SKIP	77
#define	TEST_VCPU_ID	0

int raw_stdio = 0;

static int
skip(const char *reason)
{
	fprintf(stderr, "kvm_smoke_test: skip: %s\n", reason);
	return (TEST_SKIP);
}

static int
fail_errno(const char *op)
{
	fprintf(stderr, "kvm_smoke_test: %s failed: %s\n", op,
	    strerror(errno));
	return (1);
}

static int
fail_error(const char *op, int error)
{
	fprintf(stderr, "kvm_smoke_test: %s failed: %s\n", op,
	    strerror(error));
	return (1);
}

int
main(void)
{
	struct vcpu *vcpu;
	struct vmctx *ctx;
	uint64_t x0;
	uintptr_t mem;
	int error;
	int ret;

	ctx = vm_openf("kvm-smoke-test", VMMAPI_OPEN_CREATE);
	if (ctx == NULL) {
		if (errno == ENOENT || errno == ENODEV || errno == EACCES ||
		    errno == EPERM)
			return (skip("/dev/kvm is not available"));
		return (fail_errno("vm_openf"));
	}

	mem = 0;
	error = vm_setup_memory_segment(ctx, TEST_GPA, TEST_MEM_SIZE,
	    PROT_READ | PROT_WRITE, &mem);
	if (error != 0) {
		ret = fail_error("vm_setup_memory_segment", error);
		goto out;
	}

	vcpu = vm_vcpu_open(ctx, TEST_VCPU_ID);
	if (vcpu == NULL) {
		ret = fail_errno("vm_vcpu_open");
		goto out;
	}

	if (vm_attach_vgic(ctx, TEST_GIC_DIST_BASE, TEST_GIC_DIST_SIZE,
	    TEST_GIC_REDIST_BASE, TEST_GIC_REDIST_SIZE, 0, 32, 224) != 0) {
		ret = fail_errno("vm_attach_vgic");
		goto out;
	}

	if (vm_vcpu_init(vcpu) != 0) {
		ret = fail_errno("vm_vcpu_init");
		goto out;
	}

	if (vm_set_register(vcpu, VM_REG_GUEST_PC, TEST_GPA) != 0) {
		ret = fail_errno("vm_set_register pc");
		goto out;
	}
	if (vm_set_register(vcpu, VM_REG_GUEST_X0, 0x5c0cfeedULL) != 0) {
		ret = fail_errno("vm_set_register x0");
		goto out;
	}
	if (vm_get_register(vcpu, VM_REG_GUEST_X0, &x0) != 0) {
		ret = fail_errno("vm_get_register x0");
		goto out;
	}
	if (x0 != 0x5c0cfeedULL) {
		fprintf(stderr, "kvm_smoke_test: unexpected x0 value\n");
		ret = 1;
		goto out;
	}

	ret = 0;

out:
	vm_close(ctx);
	return (ret);
}
