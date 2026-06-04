/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <support/endian.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#undef CPU_AND
#undef CPU_CLR
#undef CPU_ISSET
#undef CPU_SET
#undef CPU_ZERO

#include "vmm.h"

#include <vmmapi.h>

#include "arch/x86/inout.h"
#include "arch/x86/rtc.h"
#include "bhyverun.h"
#include "config.h"
#include "debug.h"
#include "mem.h"

#define	VM_HIGHMEM_BASE		(4 * GB)
#define	VM_MAX_MEMSLOTS		32
#define	KVM_TSS_ADDR		0xbffbd000
#define	KVM_IDENTITY_MAP_ADDR	0xbffbc000
#define	KVM_IOAPIC_PINS		24
#define	VM_LOWMEM_LIMIT		KVM_IDENTITY_MAP_ADDR

#define	CPUID_SIGNATURE		0x00000000
#define	CPUID_VERSION_INFO		0x00000001
#define	CPUID_EXTENDED_TOPOLOGY		0x0000000b
#define	CPUID_TSC_FREQ			0x00000015
#define	CPUID_EXTENDED_TOPOLOGY_V2	0x0000001f
#define	CPUID_EXTENDED_SIGNATURE	0x80000000
#define	CPUID_KVM_SIGNATURE		0x40000000
#define	CPUID_KVM_FEATURES		0x40000001
#define	CPUID_HV_SIGNATURE		0x40000000
#define	CPUID_HV_END			0x400000ff
#define	CPUID_1_EBX_APICID_SHIFT	24
#define	CPUID_1_EBX_CPU_COUNT_SHIFT	16
#define	CPUID_1_EBX_APICID_MASK		0xff000000
#define	CPUID_1_EBX_CPU_COUNT_MASK	0x00ff0000
#define	CPUID_1_ECX_X2APIC		(1U << 21)
#define	CPUID_1_ECX_HYPERVISOR		(1U << 31)
#define	CPUID_15_CRYSTAL_HZ		1000000U
#define	CPUID_15_DENOMINATOR		1000U
#define	KVM_FEATURE_NOP_IO_DELAY	1
#define	KVM_FEATURE_CLOCKSOURCE2	3
#define	KVM_FEATURE_CLOCKSOURCE_STABLE_BIT	24
#define	KVM_FEATURE_BIT(feature)	(1U << (feature))
#define	KVM_SIGNATURE_EBX		0x4b4d564bU
#define	KVM_SIGNATURE_ECX		0x564b4d56U
#define	KVM_SIGNATURE_EDX		0x0000004dU
#define	HV_SIGNATURE_EBX		0x7263694dU
#define	HV_SIGNATURE_ECX		0x666f736fU
#define	HV_SIGNATURE_EDX		0x76482074U
#define	HV_CPUID_FEATURES		0x40000003
#define	HV_CPUID_ENLIGHTENMENTS		0x40000004

extern uuid_t vm_uuid;
#define	HV_CPUID_NESTED_FEATURES	0x4000000a
#define	HV_FEATURE_TIME_REF_COUNT	(1U << 1)
#define	HV_FEATURE_SYNIC		(1U << 2)
#define	HV_FEATURE_SYNTIMER		(1U << 3)
#define	HV_FEATURE_APIC_ACCESS		(1U << 4)
#define	HV_FEATURE_HYPERCALL		(1U << 5)
#define	HV_FEATURE_VP_INDEX		(1U << 6)
#define	HV_FEATURE_REFERENCE_TSC		(1U << 9)
#define	HV_FEATURE_FREQUENCY_MSRS	(1U << 8)
#define	HV_FEATURE_STIMER_DIRECT	(1U << 19)
#define	HV_ENLIGHTENMENT_APIC_ACCESS	(1U << 3)
#define	HV_ENLIGHTENMENT_RELAXED_TIMING	(1U << 5)
#define	HV_STATUS_SUCCESS		0
#define	HV_STATUS_INVALID_HYPERCALL_CODE	2
#define	HV_STATUS_INVALID_ALIGNMENT	4
#define	HV_STATUS_INSUFFICIENT_MEMORY	11
#define	HVCALL_POST_MESSAGE		0x005c
#define	HVCALL_SIGNAL_EVENT		0x005d
#define	HVCALL_POST_DEBUG_DATA		0x0069
#define	HVCALL_RETRIEVE_DEBUG_DATA	0x006a
#define	HVCALL_RESET_DEBUG_SESSION	0x006b
#define	HV_EXT_CALL_QUERY_CAPABILITIES	0x8001
#define	HV_EXT_CALL_MAX		(HV_EXT_CALL_QUERY_CAPABILITIES + 64)
#define	HV_HYPERCALL_FAST		(1U << 16)
#define	HV_X64_MSR_GUEST_OS_ID		0x40000000
#define	HV_X64_MSR_HYPERCALL		0x40000001
#define	HV_X64_MSR_REFERENCE_TSC	0x40000021
#define	HV_X64_MSR_APIC_ASSIST_PAGE	0x40000073
#define	HV_X64_MSR_PAGE_ENABLE		1
#define	HV_X64_MSR_SCONTROL		0x40000080
#define	HV_X64_MSR_SVERSION		0x40000081
#define	HV_X64_MSR_SIEFP		0x40000082
#define	HV_X64_MSR_SIMP			0x40000083
#define	HV_X64_MSR_SINT0		0x40000090
#define	HV_X64_MSR_STIMER0_CONFIG	0x400000b0
#define	HV_SYNIC_SINT_COUNT		16
#define	HV_SYNIC_STIMER_COUNT		4
#define	HV_SYNIC_SINT_MASKED		(1ULL << 16)
#define	HV_SYNIC_VERSION_1		1

struct kvm_hv_caps {
	bool apic_access;
	bool synic;
	bool syntimer;
	bool stimer_direct;
};

enum {
	VM_MEMSEG_LOW,
	VM_MEMSEG_HIGH,
	VM_MEMSEG_COUNT,
};

struct kvm_memslot {
	uint64_t gpa;
	uint64_t prot;
	size_t len;
	void *host;
	int slot;
	bool owned;
	bool active;
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
	bool x2apic_api;
};

struct vcpu {
	struct vmctx *ctx;
	int vcpuid;
	int fd;
	struct kvm_run *run;
	size_t run_len;
	pthread_t tid;
	enum x2apic_state x2apic_state;
	bool hv_synic;
	bool hv_syntimer;
	bool tid_valid;
};

static pthread_once_t kvm_signal_once = PTHREAD_ONCE_INIT;
static __thread struct kvm_run *kvm_current_run;
static bool kvm_hyperv_warned;

static bool kvm_trace_exits(void);

static int
kvm_ioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret == -1 && errno == EINTR);

	return (ret);
}

static void
kvm_kick_handler(int signo __unused)
{
	if (kvm_current_run != NULL)
		kvm_current_run->immediate_exit = 1;
}

static void
kvm_init_kick_signal(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = kvm_kick_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL) != 0)
		err(1, "sigaction(SIGUSR1)");
}

static void
kvm_kick_vcpu(struct vcpu *vcpu)
{
	pthread_once(&kvm_signal_once, kvm_init_kick_signal);
	if (vcpu->run != NULL && vcpu->run != MAP_FAILED)
		vcpu->run->immediate_exit = 1;
	if (vcpu->tid_valid)
		(void)pthread_kill(vcpu->tid, SIGUSR1);
}

static int
kvm_check_extension(struct vmctx *ctx, int cap)
{
	return (kvm_ioctl(ctx->sys_fd, KVM_CHECK_EXTENSION,
	    (void *)(uintptr_t)cap));
}

static uint32_t
kvm_tsc_khz(struct vcpu *vcpu)
{
	int khz;

	if (kvm_check_extension(vcpu->ctx, KVM_CAP_GET_TSC_KHZ) <= 0)
		return (0);

	khz = kvm_ioctl(vcpu->fd, KVM_GET_TSC_KHZ, NULL);
	if (khz <= 0)
		return (0);

	return ((uint32_t)khz);
}

static bool
kvm_hypervisor_enabled(void)
{
	return (get_config_bool_default("x86.hypervisor", true));
}

static bool
kvm_hyperv_enabled(void)
{
	return (kvm_hypervisor_enabled() &&
	    get_config_bool_default("x86.hyperv", true));
}

static bool
kvm_hyperv_synic_enabled(void)
{
	return (kvm_hyperv_enabled() &&
	    get_config_bool_default("x86.hyperv.synic", false));
}

static bool
kvm_cfg_u32(const char *path, uint32_t *val)
{
	const char *cfg;
	char *end;
	unsigned long long n;

	cfg = get_config_value(path);
	if (cfg == NULL)
		return (false);

	errno = 0;
	n = strtoull(cfg, &end, 0);
	if (errno != 0 || end == cfg || *end != '\0' || n > UINT32_MAX) {
		warnx("invalid uint32 value %s for %s", cfg, path);
		return (false);
	}

	*val = (uint32_t)n;
	return (true);
}

static int
kvm_enable_vcpu_cap(struct vcpu *vcpu, uint32_t cap_id)
{
	struct kvm_enable_cap cap;

	memset(&cap, 0, sizeof(cap));
	cap.cap = cap_id;
	return (kvm_ioctl(vcpu->fd, KVM_ENABLE_CAP, &cap) < 0 ? errno : 0);
}

static int
kvm_enable_x2apic_api(struct vmctx *ctx)
{
	struct kvm_enable_cap cap;
	int supported;

	if (!get_config_bool_default("x86.x2apic", true))
		return (0);

	supported = kvm_check_extension(ctx, KVM_CAP_X2APIC_API);
	if (supported <= 0)
		return (ENOTSUP);

	memset(&cap, 0, sizeof(cap));
	cap.cap = KVM_CAP_X2APIC_API;
	cap.args[0] = KVM_X2APIC_API_USE_32BIT_IDS;
	if ((supported & KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK) != 0)
		cap.args[0] |= KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK;

	if (kvm_ioctl(ctx->vm_fd, KVM_ENABLE_CAP, &cap) < 0)
		return (errno);

	ctx->x2apic_api = true;
	return (0);
}

static void
kvm_init_hv(struct vcpu *vcpu)
{
	int cap_id;

	if (!kvm_hyperv_synic_enabled())
		return;

	if (!kvm_hyperv_warned) {
		warnx("x86.hyperv.synic is experimental: Hyper-V "
		    "VMBus/PostMessage is not supported");
		kvm_hyperv_warned = true;
	}

	if (kvm_check_extension(vcpu->ctx, KVM_CAP_HYPERV_SYNIC) > 0)
		cap_id = KVM_CAP_HYPERV_SYNIC;
	else if (kvm_check_extension(vcpu->ctx, KVM_CAP_HYPERV_SYNIC2) > 0)
		cap_id = KVM_CAP_HYPERV_SYNIC2;
	else
		return;

	if (kvm_enable_vcpu_cap(vcpu, (uint32_t)cap_id) != 0) {
		warnx("could not enable Hyper-V SynIC on vcpu %d",
		    vcpu->vcpuid);
		return;
	}

	vcpu->hv_synic = true;
}

static struct kvm_hv_caps
kvm_hv_caps(struct vcpu *vcpu, const struct kvm_cpuid2 *cpuid)
{
	const struct kvm_cpuid_entry2 *entry;
	struct kvm_hv_caps caps;

	memset(&caps, 0, sizeof(caps));

	for (uint32_t i = 0; i < cpuid->nent; i++) {
		entry = &cpuid->entries[i];
		if (entry->function != HV_CPUID_FEATURES)
			continue;

		caps.apic_access =
		    ((entry->eax & HV_FEATURE_APIC_ACCESS) != 0);
		caps.synic = vcpu->hv_synic &&
		    ((entry->eax & HV_FEATURE_SYNIC) != 0);
		caps.syntimer = caps.synic &&
		    ((entry->eax & HV_FEATURE_SYNTIMER) != 0);
		caps.stimer_direct = caps.syntimer &&
		    ((entry->edx & HV_FEATURE_STIMER_DIRECT) != 0);
		break;
	}

	return (caps);
}

static void
kvm_del_cpuid_range(struct kvm_cpuid2 *cpuid, uint32_t low, uint32_t high)
{
	uint32_t dst;

	dst = 0;
	for (uint32_t src = 0; src < cpuid->nent; src++) {
		if (cpuid->entries[src].function >= low &&
		    cpuid->entries[src].function <= high)
			continue;
		if (dst != src)
			cpuid->entries[dst] = cpuid->entries[src];
		dst++;
	}
	cpuid->nent = dst;
}

static struct kvm_cpuid_entry2 *
kvm_find_cpuid(struct kvm_cpuid2 *cpuid, uint32_t function, uint32_t index)
{
	struct kvm_cpuid_entry2 *entry;

	for (uint32_t i = 0; i < cpuid->nent; i++) {
		entry = &cpuid->entries[i];
		if (entry->function == function && entry->index == index)
			return (entry);
	}

	return (NULL);
}

static void
kvm_cap_cpuid(struct kvm_cpuid2 *cpuid, uint32_t basic, bool has_basic,
    uint32_t ext, bool has_ext)
{
	struct kvm_cpuid_entry2 *entry;
	uint32_t dst;

	if (has_basic) {
		entry = kvm_find_cpuid(cpuid, CPUID_SIGNATURE, 0);
		if (entry != NULL && entry->eax > basic)
			entry->eax = basic;
	}

	if (has_ext) {
		entry = kvm_find_cpuid(cpuid, CPUID_EXTENDED_SIGNATURE, 0);
		if (entry != NULL && entry->eax > ext)
			entry->eax = ext;
	}

	dst = 0;
	for (uint32_t src = 0; src < cpuid->nent; src++) {
		entry = &cpuid->entries[src];
		if (has_basic && entry->function > basic &&
		    entry->function < CPUID_KVM_SIGNATURE)
			continue;
		if (has_ext && entry->function > ext &&
		    entry->function >= CPUID_EXTENDED_SIGNATURE)
			continue;
		if (dst != src)
			cpuid->entries[dst] = cpuid->entries[src];
		dst++;
	}
	cpuid->nent = dst;
}

static struct kvm_cpuid_entry2 *
kvm_add_cpuid(struct kvm_cpuid2 *cpuid, uint32_t maxent, uint32_t function)
{
	struct kvm_cpuid_entry2 *entry;

	if (cpuid->nent >= maxent)
		return (NULL);

	entry = &cpuid->entries[cpuid->nent++];
	memset(entry, 0, sizeof(*entry));
	entry->function = function;
	return (entry);
}

static void
kvm_set_kvm_cpuid(struct kvm_cpuid2 *cpuid, uint32_t maxent,
    uint32_t signature_leaf, uint32_t features_leaf)
{
	struct kvm_cpuid_entry2 *features;
	struct kvm_cpuid_entry2 *signature;

	signature = kvm_add_cpuid(cpuid, maxent, signature_leaf);
	if (signature != NULL) {
		signature->eax = features_leaf;
		signature->ebx = KVM_SIGNATURE_EBX;
		signature->ecx = KVM_SIGNATURE_ECX;
		signature->edx = KVM_SIGNATURE_EDX;
	}

	features = kvm_add_cpuid(cpuid, maxent, features_leaf);
	if (features != NULL) {
		features->eax = KVM_FEATURE_BIT(KVM_FEATURE_NOP_IO_DELAY) |
		    KVM_FEATURE_BIT(KVM_FEATURE_CLOCKSOURCE2) |
		    KVM_FEATURE_BIT(KVM_FEATURE_CLOCKSOURCE_STABLE_BIT);
	}
}

static void
kvm_set_hv_signature(struct kvm_cpuid2 *cpuid)
{
	struct kvm_cpuid_entry2 *entry;

	for (uint32_t i = 0; i < cpuid->nent; i++) {
		entry = &cpuid->entries[i];
		if (entry->function != CPUID_HV_SIGNATURE)
			continue;

		entry->ebx = HV_SIGNATURE_EBX;
		entry->ecx = HV_SIGNATURE_ECX;
		entry->edx = HV_SIGNATURE_EDX;
		return;
	}
}

static void
kvm_filter_hv_cpuid(struct vcpu *vcpu, struct kvm_cpuid2 *cpuid)
{
	struct kvm_cpuid_entry2 *entry;
	struct kvm_hv_caps caps;
	uint32_t features;
	uint32_t x86_features;

	caps = kvm_hv_caps(vcpu, cpuid);
	vcpu->hv_syntimer = caps.syntimer;

	for (uint32_t i = 0; i < cpuid->nent; i++) {
		entry = &cpuid->entries[i];
		if (entry->function == HV_CPUID_FEATURES) {
			features = HV_FEATURE_TIME_REF_COUNT |
			    HV_FEATURE_APIC_ACCESS | HV_FEATURE_HYPERCALL |
			    HV_FEATURE_VP_INDEX | HV_FEATURE_REFERENCE_TSC;
			if (caps.synic)
				features |= HV_FEATURE_SYNIC;
			if (caps.syntimer)
				features |= HV_FEATURE_SYNTIMER;
			entry->eax &= features;
			entry->ebx = 0;

			x86_features = HV_FEATURE_FREQUENCY_MSRS;
			if (caps.stimer_direct)
				x86_features |= HV_FEATURE_STIMER_DIRECT;
			entry->edx &= x86_features;
		} else if (entry->function == HV_CPUID_ENLIGHTENMENTS ||
		    entry->function == HV_CPUID_NESTED_FEATURES) {
			if (entry->function == HV_CPUID_ENLIGHTENMENTS) {
				entry->eax &= HV_ENLIGHTENMENT_RELAXED_TIMING |
				    HV_ENLIGHTENMENT_APIC_ACCESS;
				if (!caps.apic_access)
					entry->eax &=
					    ~HV_ENLIGHTENMENT_APIC_ACCESS;
			} else
				entry->eax = 0;
			entry->ebx = 0;
			entry->ecx = 0;
			entry->edx = 0;
		}
	}
}

static int
kvm_add_hv_cpuid(struct vcpu *vcpu, struct kvm_cpuid2 *cpuid,
    uint32_t maxent)
{
	struct kvm_cpuid2 *hv_cpuid;
	size_t len;
	int error, nent;

	if (kvm_check_extension(vcpu->ctx, KVM_CAP_HYPERV_CPUID) <= 0)
		return (ENOTSUP);

	nent = 64;
again:
	len = sizeof(*hv_cpuid) + nent * sizeof(hv_cpuid->entries[0]);
	hv_cpuid = calloc(1, len);
	if (hv_cpuid == NULL)
		return (ENOMEM);
	hv_cpuid->nent = nent;

	error = 0;
	if (kvm_ioctl(vcpu->ctx->sys_fd, KVM_GET_SUPPORTED_HV_CPUID,
	    hv_cpuid) < 0)
		error = errno;

	if (error == E2BIG && nent < 256) {
		free(hv_cpuid);
		nent = 256;
		goto again;
	}
	if (error != 0) {
		free(hv_cpuid);
		return (error);
	}
	if (cpuid->nent + hv_cpuid->nent > maxent) {
		free(hv_cpuid);
		return (E2BIG);
	}

	memcpy(&cpuid->entries[cpuid->nent], hv_cpuid->entries,
	    hv_cpuid->nent * sizeof(cpuid->entries[0]));
	cpuid->nent += hv_cpuid->nent;
	kvm_filter_hv_cpuid(vcpu, cpuid);
	kvm_set_hv_signature(cpuid);

	free(hv_cpuid);
	return (0);
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

static void
kvm_fix_cpuid(struct vcpu *vcpu, struct kvm_cpuid2 *cpuid, uint32_t maxent)
{
	struct kvm_cpuid_entry2 *entry;
	struct kvm_cpuid_entry2 *tsc_entry;
	uint32_t apic_id, cpu_count, tsc_khz;
	uint32_t basic_max, ext_max;
	bool has_basic_max, has_ext_max;
	bool hypervisor;
	bool hyperv;

	has_basic_max = kvm_cfg_u32("x86.cpuid.max_basic", &basic_max);
	has_ext_max = kvm_cfg_u32("x86.cpuid.max_ext", &ext_max);
	apic_id = (uint32_t)vcpu->vcpuid;
	cpu_count = MIN((uint32_t)guest_ncpus, 0xffU);
	if (cpu_count == 0)
		cpu_count = 1;

	tsc_khz = kvm_tsc_khz(vcpu);
	tsc_entry = NULL;
	hypervisor = kvm_hypervisor_enabled();
	hyperv = kvm_hyperv_enabled();
	for (uint32_t i = 0; i < cpuid->nent; i++) {
		entry = &cpuid->entries[i];

		switch (entry->function) {
		case CPUID_SIGNATURE:
			if (entry->index == 0 &&
			    (!has_basic_max || basic_max >= CPUID_TSC_FREQ) &&
			    entry->eax < CPUID_TSC_FREQ)
				entry->eax = CPUID_TSC_FREQ;
			break;
		case CPUID_VERSION_INFO:
			entry->ebx &= ~(CPUID_1_EBX_APICID_MASK |
			    CPUID_1_EBX_CPU_COUNT_MASK);
			entry->ebx |= apic_id << CPUID_1_EBX_APICID_SHIFT;
			entry->ebx |= cpu_count << CPUID_1_EBX_CPU_COUNT_SHIFT;
			if (hypervisor)
				entry->ecx |= CPUID_1_ECX_HYPERVISOR;
			else
				entry->ecx &= ~CPUID_1_ECX_HYPERVISOR;
			if (!get_config_bool_default("x86.x2apic", true) ||
			    !vcpu->ctx->x2apic_api)
				entry->ecx &= ~CPUID_1_ECX_X2APIC;
			break;
		case CPUID_TSC_FREQ:
			if (entry->index == 0)
				tsc_entry = entry;
			break;
		case CPUID_EXTENDED_TOPOLOGY:
		case CPUID_EXTENDED_TOPOLOGY_V2:
			entry->edx = apic_id;
			break;
		default:
			break;
		}
	}

	kvm_del_cpuid_range(cpuid, CPUID_HV_SIGNATURE, CPUID_HV_END);
	if (hypervisor && (!hyperv ||
	    kvm_add_hv_cpuid(vcpu, cpuid, maxent) != 0)) {
		kvm_set_kvm_cpuid(cpuid, maxent, CPUID_KVM_SIGNATURE,
		    CPUID_KVM_FEATURES);
	}

	if (has_basic_max && basic_max < CPUID_TSC_FREQ) {
		kvm_cap_cpuid(cpuid, basic_max, has_basic_max, ext_max,
		    has_ext_max);
		return;
	}

	if (tsc_khz == 0) {
		kvm_cap_cpuid(cpuid, basic_max, has_basic_max, ext_max,
		    has_ext_max);
		return;
	}

	if (tsc_entry == NULL) {
		if (cpuid->nent >= maxent)
			goto out;
		tsc_entry = &cpuid->entries[cpuid->nent++];
		memset(tsc_entry, 0, sizeof(*tsc_entry));
		tsc_entry->function = CPUID_TSC_FREQ;
	}

	tsc_entry->eax = CPUID_15_DENOMINATOR;
	tsc_entry->ebx = tsc_khz;
	tsc_entry->ecx = CPUID_15_CRYSTAL_HZ;
	tsc_entry->edx = 0;

out:
	kvm_cap_cpuid(cpuid, basic_max, has_basic_max, ext_max, has_ext_max);
}

static int
kvm_set_cpuid(struct vcpu *vcpu)
{
	struct kvm_cpuid2 *cpuid;
	int entries, error;
	size_t len;

	entries = kvm_check_extension(vcpu->ctx, KVM_CAP_EXT_CPUID);
	if (entries <= 0 || entries > 256)
		entries = 100;
again:

	len = sizeof(*cpuid) + entries * sizeof(cpuid->entries[0]);
	cpuid = calloc(1, len);
	if (cpuid == NULL)
		return (ENOMEM);
	cpuid->nent = entries;

	error = 0;
	if (kvm_ioctl(vcpu->ctx->sys_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0)
		error = errno;
	else {
		kvm_fix_cpuid(vcpu, cpuid, (uint32_t)entries);
		if (kvm_ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid) < 0)
			error = errno;
	}

	if (error == E2BIG && entries < 256) {
		free(cpuid);
		entries = 256;
		goto again;
	}

	free(cpuid);
	return (error);
}

static int
kvm_set_mp_state(struct vcpu *vcpu, uint32_t state)
{
	struct kvm_mp_state mp_state;

	if (kvm_check_extension(vcpu->ctx, KVM_CAP_MP_STATE) <= 0)
		return (0);

	memset(&mp_state, 0, sizeof(mp_state));
	mp_state.mp_state = state;
	return (kvm_ioctl(vcpu->fd, KVM_SET_MP_STATE, &mp_state) < 0 ?
	    errno : 0);
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
	struct kvm_pit_config pit;
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

	error = kvm_enable_x2apic_api(ctx);
	if (error != 0) {
		errno = error;
		goto fail;
	}

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
	memset(&pit, 0, sizeof(pit));
	pit.flags = KVM_PIT_SPEAKER_DUMMY;
	if (kvm_ioctl(ctx->vm_fd, KVM_CREATE_PIT2, &pit) < 0)
		goto fail;
	error = kvm_set_irq_routing(ctx);
	if (error != 0) {
		errno = error;
		goto fail;
	}

	ctx->suspend_reason = VM_SUSPEND_NONE;
	SCORPI_CPU_ZERO(&ctx->active_cpus);
	SCORPI_CPU_ZERO(&ctx->suspended_cpus);
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
	for (int i = 0; i < ctx->nmemslots; i++) {
		if (ctx->memslots[i].owned)
			free(ctx->memslots[i].host);
	}
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

	if (vcpu->fd >= 0) {
		vcpu->tid = pthread_self();
		vcpu->tid_valid = true;
		return (0);
	}

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
	vcpu->tid_valid = true;
	kvm_init_hv(vcpu);
	error = kvm_set_cpuid(vcpu);
	if (error != 0) {
		vm_vcpu_deinit(vcpu);
		errno = error;
		return (-1);
	}
	error = kvm_set_mp_state(vcpu, vcpu->vcpuid == 0 ?
	    KVM_MP_STATE_RUNNABLE : KVM_MP_STATE_UNINITIALIZED);
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
	vcpu->tid_valid = false;
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
vm_set_memslot(struct vmctx *ctx, const struct kvm_memslot *slot, bool active)
{
	struct kvm_userspace_memory_region region;

	memset(&region, 0, sizeof(region));
	region.slot = slot->slot;
	if (active) {
		region.guest_phys_addr = slot->gpa;
		region.memory_size = slot->len;
		region.userspace_addr = (uintptr_t)slot->host;
#ifdef KVM_MEM_READONLY
		if ((slot->prot & PROT_WRITE) == 0)
			region.flags |= KVM_MEM_READONLY;
#endif
	}

	if (kvm_ioctl(ctx->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
		return (errno);
	return (0);
}

static int
vm_add_memslot(struct vmctx *ctx, vm_paddr_t gpa, size_t len, void *host,
    uint64_t prot, bool owned)
{
	struct kvm_memslot *slot;
	int error;
	int slotidx;

	for (int i = 0; i < ctx->nmemslots; i++) {
		struct kvm_memslot *existing = &ctx->memslots[i];
		uint64_t end = gpa + len;
		uint64_t existing_end = existing->gpa + existing->len;

		if (existing->len == 0)
			continue;
		if (gpa < existing_end && end > existing->gpa)
			return (EEXIST);
	}

	slotidx = -1;
	for (int i = 0; i < ctx->nmemslots; i++) {
		if (ctx->memslots[i].len == 0) {
			slotidx = i;
			break;
		}
	}
	if (slotidx == -1) {
		if (ctx->nmemslots >= VM_MAX_MEMSLOTS)
			return (E2BIG);
		slotidx = ctx->nmemslots++;
	}

	slot = &ctx->memslots[slotidx];
	slot->slot = slotidx;
	slot->gpa = gpa;
	slot->prot = prot;
	slot->len = len;
	slot->host = host;
	slot->owned = owned;
	slot->active = false;

	error = vm_set_memslot(ctx, slot, true);
	if (error != 0) {
		slot->gpa = 0;
		slot->prot = 0;
		slot->len = 0;
		slot->host = NULL;
		slot->owned = false;
		slot->active = false;
		return (error);
	}

	slot->active = true;
	return (0);
}

int
vm_set_memory_segment_visible(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    bool visible)
{
	struct kvm_memslot *slot;
	int error;

	for (int i = 0; i < ctx->nmemslots; i++) {
		slot = &ctx->memslots[i];
		if (slot->len != 0 && slot->gpa == gpa && slot->len == len) {
			if (slot->active == visible)
				return (0);
			error = vm_set_memslot(ctx, slot, visible);
			if (error != 0)
				return (error);
			slot->active = visible;
			return (0);
		}
	}
	return (ENOENT);
}

int
vm_setup_memory_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t len,
    uint64_t prot, uintptr_t *addr)
{
	void *host;
	bool owned;
	int error;

	if ((gpa & PAGE_MASK) != 0 || (len & PAGE_MASK) != 0 || len == 0)
		return (EINVAL);

	if ((prot & PROT_DONT_ALLOCATE) != 0) {
		if (addr == NULL || *addr == 0)
			return (EINVAL);
		host = (void *)*addr;
		owned = false;
	} else {
		if (posix_memalign(&host, PAGE_SIZE, len) != 0)
			return (ENOMEM);
		memset(host, 0, len);
		owned = true;
	}

	error = vm_add_memslot(ctx, gpa, len, host, prot, owned);
	if (error != 0) {
		if (owned)
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

static void
vm_system_memory_shm_name(char *buf, size_t len, const char *suffix)
{
	char uuid[37];

	uuid_unparse(vm_uuid, uuid);
	snprintf(buf, len, "/scorpi-%s-ram-%s", uuid, suffix);
}

static int
vm_setup_shared_system_memory_segment(struct vmctx *ctx, vm_paddr_t gpa,
    size_t len, uint64_t prot, uintptr_t *addr, const char *suffix)
{
	char name[64];
	uintptr_t mapped_addr;
	void *object;
	int error;
	int fd;

	vm_system_memory_shm_name(name, sizeof(name), suffix);
	fd = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd == -1)
		return (errno);

	if (ftruncate(fd, len) == -1) {
		error = errno;
		close(fd);
		return (error);
	}

	object = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	error = errno;
	close(fd);
	if (object == MAP_FAILED)
		return (error);

	memset(object, 0, len);
	mapped_addr = (uintptr_t)object;
	error = vm_setup_memory_segment(ctx, gpa, len,
	    prot | PROT_DONT_ALLOCATE, &mapped_addr);
	if (error != 0) {
		munmap(object, len);
		return (error);
	}
	if (addr != NULL)
		*addr = mapped_addr;
	return (0);
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
		error = vm_setup_shared_system_memory_segment(ctx, 0,
		    ctx->memsegs[VM_MEMSEG_LOW].size, PROT_READ | PROT_WRITE,
		    addr, "low");
		if (error != 0)
			return (error);
	}
	if (ctx->memsegs[VM_MEMSEG_HIGH].size > 0) {
		addr = vms == VM_MMAP_ALL ? &ctx->memsegs[VM_MEMSEG_HIGH].addr :
		    NULL;
		error = vm_setup_shared_system_memory_segment(ctx,
		    VM_HIGHMEM_BASE,
		    ctx->memsegs[VM_MEMSEG_HIGH].size,
		    PROT_READ | PROT_WRITE, addr, "high");
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
		if (!slot->active || slot->len == 0)
			continue;
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
		if (!slot->active || slot->len == 0)
			continue;
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
vm_munmap_memseg(struct vmctx *ctx, vm_paddr_t gpa, size_t len)
{
	struct kvm_memslot *slot;
	int error;

	for (int i = 0; i < ctx->nmemslots; i++) {
		slot = &ctx->memslots[i];
		if (slot->gpa != gpa || slot->len != len)
			continue;

		if (slot->active) {
			error = vm_set_memslot(ctx, slot, false);
			if (error != 0)
				return (error);
		}
		if (slot->owned)
			free(slot->host);
		slot->gpa = 0;
		slot->prot = 0;
		slot->len = 0;
		slot->host = NULL;
		slot->owned = false;
		slot->active = false;
		return (0);
	}

	return (ENOENT);
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

static int
kvm_set_msr(struct vcpu *vcpu, uint32_t index, uint64_t data)
{
	union {
		struct kvm_msrs msrs;
		char buf[sizeof(struct kvm_msrs) +
		    sizeof(struct kvm_msr_entry)];
	} u;
	int ret;

	memset(&u, 0, sizeof(u));
	u.msrs.nmsrs = 1;
	u.msrs.entries[0].index = index;
	u.msrs.entries[0].data = data;

	ret = kvm_ioctl(vcpu->fd, KVM_SET_MSRS, &u.msrs);
	if (ret < 0)
		return (errno);
	if (ret != 1)
		return (ENXIO);

	return (0);
}

static int
kvm_get_msr(struct vcpu *vcpu, uint32_t index, uint64_t *data)
{
	union {
		struct kvm_msrs msrs;
		char buf[sizeof(struct kvm_msrs) +
		    sizeof(struct kvm_msr_entry)];
	} u;
	int ret;

	memset(&u, 0, sizeof(u));
	u.msrs.nmsrs = 1;
	u.msrs.entries[0].index = index;

	ret = kvm_ioctl(vcpu->fd, KVM_GET_MSRS, &u.msrs);
	if (ret < 0)
		return (errno);
	if (ret != 1)
		return (ENXIO);

	*data = u.msrs.entries[0].data;
	return (0);
}

static void
kvm_clear_hv_page(struct vcpu *vcpu, uint64_t msr)
{
	void *page;
	vm_paddr_t gpa;

	if ((msr & HV_X64_MSR_PAGE_ENABLE) == 0)
		return;

	gpa = msr & ~(vm_paddr_t)PAGE_MASK;
	page = vm_map_gpa(vcpu->ctx, gpa, PAGE_SIZE);
	if (page != NULL)
		memset(page, 0, PAGE_SIZE);
}

static int
kvm_reset_hv_msrs(struct vcpu *vcpu)
{
	int error;
	uint64_t msr;

	if (!kvm_hyperv_enabled())
		return (0);

	if (vcpu->vcpuid == 0) {
		if ((error = kvm_set_msr(vcpu, HV_X64_MSR_GUEST_OS_ID,
		    0)) != 0)
			return (error);
		if ((error = kvm_set_msr(vcpu, HV_X64_MSR_HYPERCALL,
		    0)) != 0)
			return (error);
		if ((error = kvm_set_msr(vcpu, HV_X64_MSR_REFERENCE_TSC,
		    0)) != 0)
			return (error);
	}

	if (kvm_get_msr(vcpu, HV_X64_MSR_APIC_ASSIST_PAGE, &msr) == 0)
		kvm_clear_hv_page(vcpu, msr);
	if ((error = kvm_set_msr(vcpu, HV_X64_MSR_APIC_ASSIST_PAGE, 0)) != 0)
		return (error);

	if (!vcpu->hv_synic)
		return (0);

	if (kvm_get_msr(vcpu, HV_X64_MSR_SIEFP, &msr) == 0)
		kvm_clear_hv_page(vcpu, msr);
	if (kvm_get_msr(vcpu, HV_X64_MSR_SIMP, &msr) == 0)
		kvm_clear_hv_page(vcpu, msr);

	if ((error = kvm_set_msr(vcpu, HV_X64_MSR_SVERSION,
	    HV_SYNIC_VERSION_1)) != 0)
		return (error);
	if ((error = kvm_set_msr(vcpu, HV_X64_MSR_SCONTROL, 0)) != 0)
		return (error);
	if ((error = kvm_set_msr(vcpu, HV_X64_MSR_SIEFP, 0)) != 0)
		return (error);
	if ((error = kvm_set_msr(vcpu, HV_X64_MSR_SIMP, 0)) != 0)
		return (error);
	for (uint32_t i = 0; i < HV_SYNIC_SINT_COUNT; i++) {
		if ((error = kvm_set_msr(vcpu, HV_X64_MSR_SINT0 + i,
		    HV_SYNIC_SINT_MASKED)) != 0)
			return (error);
	}

	if (!vcpu->hv_syntimer)
		return (0);

	for (uint32_t i = 0; i < HV_SYNIC_STIMER_COUNT; i++) {
		if ((error = kvm_set_msr(vcpu, HV_X64_MSR_STIMER0_CONFIG +
		    i * 2, 0)) != 0)
			return (error);
		if ((error = kvm_set_msr(vcpu, HV_X64_MSR_STIMER0_CONFIG +
		    i * 2 + 1, 0)) != 0)
			return (error);
	}

	return (0);
}

int
vcpu_reset(struct vcpu *vcpu)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	int error;

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
	if ((error = vm_put_regs(vcpu, &regs)) != 0)
		return (error);
	if ((error = kvm_reset_hv_msrs(vcpu)) != 0)
		return (error);
	return (kvm_set_mp_state(vcpu, vcpu->vcpuid == 0 ?
	    KVM_MP_STATE_RUNNABLE : KVM_MP_STATE_UNINITIALIZED));
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

static const char *
kvm_hyperv_hcall_name(uint16_t code)
{
	switch (code) {
	case HVCALL_POST_MESSAGE:
		return ("POST_MESSAGE");
	case HVCALL_SIGNAL_EVENT:
		return ("SIGNAL_EVENT");
	case HVCALL_POST_DEBUG_DATA:
		return ("POST_DEBUG_DATA");
	case HVCALL_RETRIEVE_DEBUG_DATA:
		return ("RETRIEVE_DEBUG_DATA");
	case HVCALL_RESET_DEBUG_SESSION:
		return ("RESET_DEBUG_SESSION");
	default:
		if (code >= HV_EXT_CALL_QUERY_CAPABILITIES &&
		    code <= HV_EXT_CALL_MAX)
			return ("EXTENDED");
		return ("UNKNOWN");
	}
}

static uint64_t
kvm_handle_hyperv_ext_hcall(struct vcpu *vcpu, uint16_t code,
    uint64_t outgpa)
{
	uint64_t *caps;

	if (code != HV_EXT_CALL_QUERY_CAPABILITIES)
		return (HV_STATUS_INVALID_HYPERCALL_CODE);
	if ((outgpa & (sizeof(*caps) - 1)) != 0)
		return (HV_STATUS_INVALID_ALIGNMENT);

	caps = vm_map_gpa(vcpu->ctx, outgpa, sizeof(*caps));
	if (caps == NULL)
		return (HV_STATUS_INSUFFICIENT_MEMORY);

	/*
	 * HvExtCallQueryCapabilities returns the set of extended hypercalls
	 * supported beyond the query itself.  Scorpi currently supports none.
	 */
	*caps = htole64(0);
	return (HV_STATUS_SUCCESS);
}

static int
kvm_handle_hyperv(struct vcpu *vcpu)
{
	struct kvm_hyperv_exit *hv;
	uint16_t code;

	hv = &vcpu->run->hyperv;
	switch (hv->type) {
	case KVM_EXIT_HYPERV_SYNIC:
		return (0);
	case KVM_EXIT_HYPERV_HCALL:
		code = hv->u.hcall.input & 0xffff;
		if (kvm_trace_exits()) {
			PRINTLN("vcpu %d Hyper-V hcall %s(%#x) fast=%u in=%#llx out=%#llx",
			    vcpu_id(vcpu), kvm_hyperv_hcall_name(code), code,
			    (hv->u.hcall.input & HV_HYPERCALL_FAST) != 0,
			    (unsigned long long)hv->u.hcall.params[0],
			    (unsigned long long)hv->u.hcall.params[1]);
		}
		if (code >= HV_EXT_CALL_QUERY_CAPABILITIES &&
		    code <= HV_EXT_CALL_MAX) {
			hv->u.hcall.result = kvm_handle_hyperv_ext_hcall(vcpu,
			    code, hv->u.hcall.params[1]);
		} else
			hv->u.hcall.result = HV_STATUS_INVALID_HYPERCALL_CODE;
		return (0);
	default:
		warnx("unexpected KVM Hyper-V exit type %u", hv->type);
		errno = EOPNOTSUPP;
		return (-1);
	}
}

static void
kvm_set_suspended_exit(struct vm_exit *vme, enum vm_suspend_how how)
{
	vme->exitcode = VM_EXITCODE_SUSPENDED;
	vme->u.suspended.how = how;
}

static int
kvm_handle_system_event(struct vcpu *vcpu, struct kvm_run *run,
    struct vm_exit *vme)
{
	switch (run->system_event.type) {
	case KVM_SYSTEM_EVENT_SHUTDOWN:
		if (vm_suspend(vcpu->ctx, VM_SUSPEND_POWEROFF) != 0)
			return (-1);
		kvm_set_suspended_exit(vme, VM_SUSPEND_POWEROFF);
		return (0);
	case KVM_SYSTEM_EVENT_RESET:
		if (vm_suspend(vcpu->ctx, VM_SUSPEND_RESET) != 0)
			return (-1);
		kvm_set_suspended_exit(vme, VM_SUSPEND_RESET);
		return (0);
	case KVM_SYSTEM_EVENT_CRASH:
		kvm_set_suspended_exit(vme, VM_SUSPEND_HALT);
		return (0);
	default:
		warnx("unexpected KVM system event %u",
		    run->system_event.type);
		errno = ENOTSUP;
		return (-1);
	}
}

static const char *
kvm_exit_name(uint32_t reason)
{
	switch (reason) {
	case KVM_EXIT_IO:
		return ("IO");
	case KVM_EXIT_MMIO:
		return ("MMIO");
	case KVM_EXIT_HLT:
		return ("HLT");
	case KVM_EXIT_INTR:
		return ("INTR");
	case KVM_EXIT_SHUTDOWN:
		return ("SHUTDOWN");
	case KVM_EXIT_FAIL_ENTRY:
		return ("FAIL_ENTRY");
	case KVM_EXIT_INTERNAL_ERROR:
		return ("INTERNAL_ERROR");
	case KVM_EXIT_HYPERV:
		return ("HYPERV");
	case KVM_EXIT_SYSTEM_EVENT:
		return ("SYSTEM_EVENT");
	default:
		return ("OTHER");
	}
}

static bool
kvm_trace_exits(void)
{
	const char *env;

	env = getenv("SCORPI_TRACE_KVM_EXITS");
	if (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0)
		return (true);

	return (get_config_bool_default("x86.trace_exits", false));
}

static void
kvm_trace_exit(struct vcpu *vcpu)
{
	static __thread struct {
		bool initialized;
		uint64_t total;
		uint64_t reason[64];
		uint64_t io_count;
		uint64_t mmio_count;
		uint16_t io_port;
		uint64_t mmio_addr;
		struct timespec last;
	} trace;
	struct timespec now;
	uint32_t reason;
	uint64_t elapsed_ns;
	uint64_t max_count;
	uint64_t rip;
	uint32_t max_reason;

	if (!kvm_trace_exits())
		return;

	reason = vcpu->run->exit_reason;
	if (!trace.initialized) {
		clock_gettime(CLOCK_MONOTONIC, &trace.last);
		trace.initialized = true;
	}

	trace.total++;
	if (reason < nitems(trace.reason))
		trace.reason[reason]++;

	if (reason == KVM_EXIT_IO) {
		trace.io_count++;
		trace.io_port = vcpu->run->io.port;
	} else if (reason == KVM_EXIT_MMIO) {
		trace.mmio_count++;
		trace.mmio_addr = vcpu->run->mmio.phys_addr;
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed_ns = (now.tv_sec - trace.last.tv_sec) * 1000000000ULL +
	    (now.tv_nsec - trace.last.tv_nsec);
	if (elapsed_ns < 1000000000ULL)
		return;

	max_reason = 0;
	max_count = 0;
	for (uint32_t i = 0; i < nitems(trace.reason); i++) {
		if (trace.reason[i] > max_count) {
			max_count = trace.reason[i];
			max_reason = i;
		}
	}

	rip = 0;
	(void)vm_get_register(vcpu, VM_REG_GUEST_RIP, &rip);

	PRINTLN("vcpu %d exits: total=%llu top=%s(%u)=%llu io=%llu last_port=%#x mmio=%llu last_addr=%#llx rip=%#llx",
	    vcpu_id(vcpu), (unsigned long long)trace.total,
	    kvm_exit_name(max_reason), max_reason, (unsigned long long)max_count,
	    (unsigned long long)trace.io_count, trace.io_port,
	    (unsigned long long)trace.mmio_count,
	    (unsigned long long)trace.mmio_addr, (unsigned long long)rip);

	memset(trace.reason, 0, sizeof(trace.reason));
	trace.total = 0;
	trace.io_count = 0;
	trace.mmio_count = 0;
	trace.last = now;
}

int
vm_run(struct vcpu *vcpu, struct vm_run *vmrun)
{
	struct vm_exit *vme;
	int ret;

	vme = vmrun->vm_exit;
	for (;;) {
		if (vcpu->ctx->suspend_reason != VM_SUSPEND_NONE) {
			kvm_set_suspended_exit(vme, vcpu->ctx->suspend_reason);
			return (0);
		}
		vcpu->run->immediate_exit = 0;
		if (vcpu->ctx->suspend_reason != VM_SUSPEND_NONE) {
			kvm_set_suspended_exit(vme, vcpu->ctx->suspend_reason);
			return (0);
		}

		kvm_current_run = vcpu->run;
		ret = ioctl(vcpu->fd, KVM_RUN, NULL);
		kvm_current_run = NULL;
		if (ret < 0) {
			if (vcpu->ctx->suspend_reason != VM_SUSPEND_NONE) {
				kvm_set_suspended_exit(vme,
				    vcpu->ctx->suspend_reason);
				return (0);
			}
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return (-1);
		}

		if (vcpu->ctx->suspend_reason != VM_SUSPEND_NONE) {
			kvm_set_suspended_exit(vme, vcpu->ctx->suspend_reason);
			return (0);
		}

		kvm_trace_exit(vcpu);

		switch (vcpu->run->exit_reason) {
		case KVM_EXIT_IO:
			if (kvm_handle_io(vcpu) != 0)
				return (-1);
			if (vcpu->ctx->suspend_reason != VM_SUSPEND_NONE) {
				kvm_set_suspended_exit(vme,
				    vcpu->ctx->suspend_reason);
				return (0);
			}
			break;
		case KVM_EXIT_MMIO:
			if (kvm_handle_mmio(vcpu) != 0)
				return (-1);
			if (vcpu->ctx->suspend_reason != VM_SUSPEND_NONE) {
				kvm_set_suspended_exit(vme,
				    vcpu->ctx->suspend_reason);
				return (0);
			}
			break;
		case KVM_EXIT_HYPERV:
			if (kvm_handle_hyperv(vcpu) != 0)
				return (-1);
			break;
		case KVM_EXIT_SYSTEM_EVENT:
			return (kvm_handle_system_event(vcpu, vcpu->run, vme));
		case KVM_EXIT_HLT:
			vme->exitcode = VM_EXITCODE_HLT;
			vme->rip = vcpu->run->hw.hardware_exit_reason;
			return (0);
		case KVM_EXIT_SHUTDOWN:
			if (vm_suspend(vcpu->ctx, VM_SUSPEND_RESET) != 0)
				return (-1);
			kvm_set_suspended_exit(vme, VM_SUSPEND_RESET);
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
	if (how != VM_SUSPEND_NONE)
		return (vm_suspend_all_cpus(ctx));
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
vm_get_x2apic_state(struct vcpu *vcpu, enum x2apic_state *s)
{
	*s = vcpu->x2apic_state;
	return (0);
}

int
vm_set_x2apic_state(struct vcpu *vcpu, enum x2apic_state s)
{
	if (s >= X2APIC_STATE_LAST)
		return (EINVAL);
	if (s == X2APIC_ENABLED && !vcpu->ctx->x2apic_api)
		return (EOPNOTSUPP);

	vcpu->x2apic_state = s;
	return (0);
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
	return (vm_gla2gpa_nofault(vcpu, paging, gla, prot, gpa, fault));
}

int
vm_gla2gpa_nofault(struct vcpu *vcpu, struct vm_guest_paging *paging __unused,
    uint64_t gla, int prot __unused, uint64_t *gpa, int *fault)
{
	struct kvm_translation tr;

	memset(&tr, 0, sizeof(tr));
	tr.linear_address = gla;
	if (kvm_ioctl(vcpu->fd, KVM_TRANSLATE, &tr) < 0)
		return (errno);

	if (fault != NULL)
		*fault = tr.valid ? 0 : 1;
	if (!tr.valid)
		return (EFAULT);

	*gpa = tr.physical_address;
	return (0);
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
		SCORPI_CPU_ZERO(cpus);
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
	SCORPI_CPU_SET_ATOMIC(vcpu->vcpuid, &vcpu->ctx->active_cpus);
	SCORPI_CPU_CLR_ATOMIC(vcpu->vcpuid, &vcpu->ctx->suspended_cpus);
	return (0);
}

int
vm_suspend_cpu(struct vcpu *vcpu)
{
	SCORPI_CPU_CLR_ATOMIC(vcpu->vcpuid, &vcpu->ctx->active_cpus);
	SCORPI_CPU_SET_ATOMIC(vcpu->vcpuid, &vcpu->ctx->suspended_cpus);
	kvm_kick_vcpu(vcpu);
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
    uint8_t value)
{
	return (x86_rtc_write(offset, value));
}

int
vm_rtc_read(struct vmctx *ctx __unused, int offset __unused, uint8_t *retval)
{
	return (x86_rtc_read(offset, retval));
}

int
vm_rtc_settime(struct vmctx *ctx __unused, time_t secs)
{
	return (x86_rtc_settime(secs));
}

int
vm_rtc_gettime(struct vmctx *ctx __unused, time_t *secs)
{
	return (x86_rtc_gettime(secs));
}
