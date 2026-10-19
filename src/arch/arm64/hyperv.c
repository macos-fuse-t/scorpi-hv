/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ARM64 TLFS crash notification subset. Reference layouts:
 * https://learn.microsoft.com/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
 * https://learn.microsoft.com/virtualization/hyper-v-on-windows/tlfs/partition-properties
 * Linux arch/arm64/hyperv/hv_core.c also documents the fast register ABI.
 */
#include <arch/arm64/hyperv.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "debug.h"

#define HV_GET_REGISTERS 0x50
#define HV_SET_REGISTERS 0x51
#define HV_SUCCESS 0
#define HV_INVALID_CODE 2
#define HV_INVALID_INPUT 3
#define HV_INVALID_ALIGNMENT 4
#define HV_INVALID_PARAMETER 5
#define HV_ACCESS_DENIED 6
#define HV_INVALID_PARTITION 13
#define HV_INVALID_VP 14
#define HV_NOTIFY (UINT64_C(1) << 63)
#define HV_PARTITION_SELF UINT64_MAX
#define HV_VP_SELF UINT32_C(0xfffffffe)
#define HV_PAGE_SIZE 4096

struct hv_value { uint64_t low, high; };
struct hv_header { uint64_t partition; uint32_t vp, vtl_reserved; };
struct hv_set_element { uint32_t name, reserved[3]; struct hv_value value; };
_Static_assert(sizeof(struct hv_header) == 16, "Hyper-V input header");
_Static_assert(sizeof(struct hv_set_element) == 32, "Hyper-V register association");

int
arm64_hyperv_init(struct arm64_hyperv *hv)
{
	memset(hv, 0, sizeof(*hv));
	return pthread_mutex_init(&hv->lock, NULL);
}

void
arm64_hyperv_reset(struct arm64_hyperv *hv)
{
	pthread_mutex_lock(&hv->lock);
	hv->guest_os_id = 0;
	hv->partition_info_page = 0;
	hv->register_errors = 0;
	memset(hv->vp_enlightened, 0, sizeof(hv->vp_enlightened));
	memset(hv->synthetic_vectors, 0, sizeof(hv->synthetic_vectors));
	memset(hv->crash_params, 0, sizeof(hv->crash_params));
	hv->discovered = false;
	pthread_mutex_unlock(&hv->lock);
}

void
arm64_hyperv_destroy(struct arm64_hyperv *hv)
{
	pthread_mutex_destroy(&hv->lock);
}

static void
announce(struct arm64_hyperv *hv)
{
	if (!hv->discovered) {
		PRINTLN("Hyper-V ARM64: guest discovered crash notification interface");
		hv->discovered = true;
	}
}

static unsigned int
read_register(struct arm64_hyperv *hv, uint32_t name, struct hv_value *v,
    unsigned int cpu, unsigned int cpu_count)
{
	*v = (struct hv_value){0, 0};
	switch (name) {
	case 0x100: /* HvRegisterHypervisorVersion */
		/* Interface version only; feature bits govern available services. */
		v->low = UINT64_C(10) << 48;
		break;
	case 0x200: /* HvRegisterPrivilegesAndFeaturesInfo */
		/* AccessVpIndex; no SynIC, timers, VMBus, or enlightened TLB/IPI. */
		v->low = UINT64_C(1) << 6;
		/* ARM64 crash capability is bit 72, not the x64 EDX bit 10. */
		v->high = UINT64_C(1) << 8;
		announce(hv);
		break;
	case 0x201: /* HvRegisterFeaturesInfo */
		v->low = UINT64_C(0xffffffff) << 32; /* never notify spin waits */
		break;
	case 0x202: /* HvRegisterImplementationLimitsInfo */
		v->low = cpu_count | ((uint64_t)cpu_count << 32);
		break;
	case 0x203: /* HardwareFeaturesInfo */
	case 0x204: /* CpuManagementFeaturesInfo */
	case 0x205: /* PasidFeaturesInfo */
	case 0x206: /* SkipLevelFeaturesInfo */
	case 0x207: /* NestedVirtFeaturesInfo */
	case 0x208: /* IptFeaturesInfo */
	case 0x209: /* IsolationConfiguration: no VSM/isolation support */
		break;
	case 0x210: case 0x211: case 0x212: case 0x213: case 0x214:
		v->low = hv->crash_params[name - 0x210];
		break;
	case 0x215: /* GuestCrashCtl: only CrashNotify is supported. */
		v->low = HV_NOTIFY;
		break;
	case 0x90002: /* GuestOsId */
		v->low = hv->guest_os_id;
		break;
	case 0x40400: /* Legacy ARM64 synthetic exception vector base. */
		v->low = hv->synthetic_vectors[cpu];
		break;
	case 0x90016: /* Legacy per-VP enlightenment enable. */
		v->low = hv->vp_enlightened[cpu];
		break;
	case 0x90015: /* PartitionInfoPage: required by Windows ARM64 phase 0. */
		v->low = hv->partition_info_page;
		break;
	case 0x90006: /* HypervisorInterface: vendor-neutral Hv#1 */
		v->low = UINT32_C(0x31237648);
		break;
	case 0x90003: /* VpIndex */
		v->low = cpu;
		break;
	default:
		return HV_INVALID_PARAMETER;
	}
	return HV_SUCCESS;
}

static unsigned int
write_register(struct arm64_hyperv *hv, uint32_t name, struct hv_value v,
    unsigned int cpu, arm64_hyperv_map_fn map, void *arg)
{
	if (v.high != 0)
		return HV_INVALID_PARAMETER;
	if (name >= 0x210 && name <= 0x214) {
		hv->crash_params[name - 0x210] = v.low;
		return HV_SUCCESS;
	}
	if (name == 0x90002) {
		if (cpu != 0)
			return HV_ACCESS_DENIED;
		hv->guest_os_id = v.low;
		PRINTLN("Hyper-V ARM64: guest OS ID=0x%016" PRIx64, v.low);
		return HV_SUCCESS;
	}
	if (name == 0x40400) {
		/* Windows registers HvlArm64SyntheticExceptionVectors during boot.
		 * This is separate from architectural VBAR_EL1. Store the callback;
		 * with no synthetic events advertised there is nothing to deliver.
		 */
		if (v.low & 0x7ff)
			return HV_INVALID_PARAMETER;
		hv->synthetic_vectors[cpu] = v.low;
		return HV_SUCCESS;
	}
	if (name == 0x90016) {
		/* HvlEnlightenProcessor writes 1 even without optional services.
		 * Track the per-VP opt-in; no pending hypervisor actions exist.
		 */
		if (v.low > 1)
			return HV_INVALID_PARAMETER;
		hv->vp_enlightened[cpu] = v.low != 0;
		return HV_SUCCESS;
	}
	if (name == 0x90015) {
		/* Bit 0 enables a guest-supplied 4 KiB partition information page.
		 * Windows reads its first word to decide whether a deferred
		 * hypervisor action is pending. We have none, so it remains zero.
		 * Never acknowledge registration without validating the whole page.
		 */
		if (v.low & 0xffe)
			return HV_INVALID_PARAMETER;
		if (v.low & 1) {
			void *page = map(arg, v.low & ~UINT64_C(0xfff), HV_PAGE_SIZE);
			if (page == NULL)
				return HV_INVALID_PARAMETER;
			memset(page, 0, HV_PAGE_SIZE);
		}
		hv->partition_info_page = v.low;
		return HV_SUCCESS;
	}
	if (name != 0x215)
		return HV_INVALID_PARAMETER;
	/* Accept PreOSId and NoCrashDump metadata, but not CrashMessage. */
	if (v.low & ((UINT64_C(1) << 62) | ((UINT64_C(1) << 58) - 1)))
		return HV_INVALID_PARAMETER;
	if (v.low & HV_NOTIFY) {
		EPRINTLN("Hyper-V guest crash: vcpu=%u guestOsId=0x%016" PRIx64
		    " control=0x%016" PRIx64 " P0=0x%016" PRIx64
		    " P1=0x%016" PRIx64 " P2=0x%016" PRIx64
		    " P3=0x%016" PRIx64 " P4=0x%016" PRIx64,
		    cpu, hv->guest_os_id, v.low, hv->crash_params[0],
		    hv->crash_params[1], hv->crash_params[2],
		    hv->crash_params[3], hv->crash_params[4]);
		/* Do not stop/reset: Windows must finish its own dump procedure. */
	}
	return HV_SUCCESS;
}

static void *
map_parameters(arm64_hyperv_map_fn map, void *arg, uint64_t gpa, size_t size)
{
	if ((gpa & 7) || size > HV_PAGE_SIZE - (gpa & (HV_PAGE_SIZE - 1)) ||
	    gpa > UINT64_MAX - size)
		return NULL;
	return map(arg, gpa, size);
}

static uint64_t
register_call(struct arm64_hyperv *hv, uint64_t x[18], unsigned int first,
    uint64_t control, unsigned int cpu, unsigned int cpu_count,
    arm64_hyperv_map_fn map, void *arg)
{
	unsigned int code = control & 0xffff, count = (control >> 32) & 0xfff;
	unsigned int start = (control >> 48) & 0xfff, done = start, status;
	bool set = code == HV_SET_REGISTERS, fast = (control & (1u << 16)) != 0;
	struct hv_header header;
	struct hv_value value;
	struct hv_set_element element;
	uint32_t name;
	unsigned char input[HV_PAGE_SIZE];
	unsigned char *output = NULL;
	size_t input_size, output_size, output_offset = 0;
	const uint64_t allowed = UINT64_C(0x0fff0fff0001ffff);
	if (code != HV_GET_REGISTERS && code != HV_SET_REGISTERS)
		return HV_INVALID_CODE;
	if ((control & ~allowed) || count == 0 || start >= count)
		return HV_INVALID_INPUT;
	input_size = sizeof(header) + count * (set ? sizeof(element) : sizeof(name));
	output_size = set ? 0 : count * sizeof(value);
	if (input_size > sizeof(input) || output_size > HV_PAGE_SIZE)
		return HV_INVALID_INPUT;
	if (fast) {
		/* Register values occupy 16-byte slots, including fast GET output. */
		output_offset = (input_size + 15) & ~(size_t)15;
		if (input_size > 128 || (!set && output_offset + output_size > 128))
			return HV_INVALID_INPUT;
		/* Windows' HVC #1 thunk packs output at the end of X1..X16.
		 * SMCCC GET instead places it after the padded input (X6/X7
		 * for one register), as used by Linux hv_get_vpreg_128(). */
		if (!set && first == 1)
			output_offset = 128 - output_size;
		memcpy(input, &x[first], input_size);
		if (!set)
			output = (unsigned char *)&x[first] + output_offset;
	} else {
		void *in = map_parameters(map, arg, x[first], input_size);
		if (in == NULL)
			return HV_INVALID_ALIGNMENT;
		if (!set) {
			output = map_parameters(map, arg, x[first + 1], output_size);
			if (output == NULL || (x[first] < x[first + 1] + output_size &&
			    x[first + 1] < x[first] + input_size))
				return HV_INVALID_ALIGNMENT;
		}
		/* Snapshot guest-owned input before parsing or writing output. */
		memcpy(input, in, input_size);
	}
	memcpy(&header, input, sizeof(header));
	if (header.partition != HV_PARTITION_SELF)
		return HV_INVALID_PARTITION;
	if (header.vp != HV_VP_SELF && header.vp != cpu)
		return HV_INVALID_VP;
	if (header.vtl_reserved != 0)
		return HV_INVALID_INPUT;
	status = HV_SUCCESS;
	for (; done < count; done++) {
		if (set) {
			memcpy(&element, input + sizeof(header) + done * sizeof(element),
			    sizeof(element));
			if (element.reserved[0] || element.reserved[1] || element.reserved[2]) {
				status = HV_INVALID_PARAMETER;
				break;
			}
			status = write_register(hv, element.name, element.value, cpu, map, arg);
		} else {
			memcpy(&name, input + sizeof(header) + done * sizeof(name), sizeof(name));
			status = read_register(hv, name, &value, cpu, cpu_count);
			if (status == HV_SUCCESS)
				memcpy(output + done * sizeof(value), &value, sizeof(value));
		}
		if (status != HV_SUCCESS) {
			uint32_t failed_name = set ? element.name : name;
			unsigned int i;
			for (i = 0; i < hv->register_errors; i++)
				if (hv->failed_registers[i] == failed_name) break;
			if (i == hv->register_errors && i < 16) {
				hv->failed_registers[hv->register_errors++] = failed_name;
				PRINTLN("Hyper-V ARM64: %s register=0x%x failed status=%u vcpu=%u",
				    set ? "SET" : "GET", failed_name, status, cpu);
			}
			break;
		}
	}
	return status | ((uint64_t)done << 32);
}

bool
arm64_hyperv_call(struct arm64_hyperv *hv, unsigned int imm, uint64_t x[18],
    unsigned int cpu, unsigned int cpu_count, arm64_hyperv_map_fn map, void *arg)
{
	bool uid = imm == 0 && (x[0] == UINT64_C(0x8600ff01) ||
	    x[0] == UINT64_C(0xc600ff01));
	if (!uid && imm != 1 && !(imm == 0 && x[0] == UINT64_C(0x46000001)))
		return false;
	pthread_mutex_lock(&hv->lock);
	if (uid) {
		x[0] = 0x4d32ba58; x[1] = 0xcd244764;
		x[2] = 0x8eef6c75; x[3] = 0x16597024;
		announce(hv);
	} else if (cpu >= sizeof(hv->vp_enlightened)) {
		x[0] = HV_INVALID_VP;
	} else {
		unsigned int control_reg = imm == 1 ? 0 : 1;
		x[0] = register_call(hv, x, control_reg + 1, x[control_reg],
		    cpu, cpu_count, map, arg);
	}
	pthread_mutex_unlock(&hv->lock);
	return true;
}
