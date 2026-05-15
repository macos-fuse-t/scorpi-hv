/* Linux/KVM bring-up smoke test for the ARM64 port. */

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KVM_API_VERSION_EXPECTED 12
#define TEST_GPA 0x40000000ULL
#define TEST_MEM_SIZE 0x10000UL
#define TEST_VCPU_ID 0
#define TEST_SKIP 77

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
fail_msg(const char *msg)
{
	fprintf(stderr, "kvm_smoke_test: %s\n", msg);
	return (1);
}

static int
set_u64_reg(int vcpu_fd, uint64_t id, uint64_t value)
{
	struct kvm_one_reg reg;

	reg.id = id;
	reg.addr = (uintptr_t)&value;
	if (ioctl(vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		return (-1);
	return (0);
}

static uint64_t
arm64_core_reg_id(size_t reg_offset)
{
	return (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
	    reg_offset);
}

int
main(void)
{
	struct kvm_userspace_memory_region memreg;
	struct kvm_vcpu_init init;
	void *mem;
	void *run;
	int api_version;
	int kvm_fd;
	int mmap_size;
	int ret;
	int vcpu_fd;
	int vm_fd;

	kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm_fd < 0) {
		if (errno == ENOENT || errno == ENODEV)
			return (skip("/dev/kvm is not available"));
		return (fail_errno("open /dev/kvm"));
	}

	api_version = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
	if (api_version != KVM_API_VERSION_EXPECTED)
		return (fail_msg("unexpected KVM API version"));

	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		return (fail_errno("KVM_CREATE_VM"));

	mem = mmap(NULL, TEST_MEM_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED)
		return (fail_errno("mmap guest memory"));

	memset(&memreg, 0, sizeof(memreg));
	memreg.slot = 0;
	memreg.guest_phys_addr = TEST_GPA;
	memreg.memory_size = TEST_MEM_SIZE;
	memreg.userspace_addr = (uintptr_t)mem;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memreg) < 0)
		return (fail_errno("KVM_SET_USER_MEMORY_REGION"));

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, TEST_VCPU_ID);
	if (vcpu_fd < 0)
		return (fail_errno("KVM_CREATE_VCPU"));

	mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size <= 0)
		return (fail_errno("KVM_GET_VCPU_MMAP_SIZE"));

	run = mmap(NULL, (size_t)mmap_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, vcpu_fd, 0);
	if (run == MAP_FAILED)
		return (fail_errno("mmap vcpu run state"));

	memset(&init, 0, sizeof(init));
	if (ioctl(vm_fd, KVM_ARM_PREFERRED_TARGET, &init) < 0)
		return (fail_errno("KVM_ARM_PREFERRED_TARGET"));
	if (ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, &init) < 0)
		return (fail_errno("KVM_ARM_VCPU_INIT"));

	ret = set_u64_reg(vcpu_fd,
	    arm64_core_reg_id(KVM_REG_ARM_CORE_REG(regs.pc)), TEST_GPA);
	if (ret < 0)
		return (fail_errno("KVM_SET_ONE_REG pc"));
	ret = set_u64_reg(vcpu_fd,
	    arm64_core_reg_id(KVM_REG_ARM_CORE_REG(regs.regs[0])),
	    0x5c0cfeedULL);
	if (ret < 0)
		return (fail_errno("KVM_SET_ONE_REG x0"));

	munmap(run, (size_t)mmap_size);
	close(vcpu_fd);
	munmap(mem, TEST_MEM_SIZE);
	close(vm_fd);
	close(kvm_fd);
	return (0);
}
