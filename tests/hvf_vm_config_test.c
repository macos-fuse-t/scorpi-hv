/* SPDX-License-Identifier: BSD-2-Clause */
#define hv_vm_config_get_max_ipa_size test_get_max_ipa_size
#define hv_vm_config_set_ipa_size test_set_ipa_size
#define hv_vm_create test_vm_create
#include <assert.h>
#include "../src/libvmm/hvf/vmm.c"

static hv_vm_config_t expected_config;
static hv_return_t query_result, set_result, create_result;
static unsigned int queries, sets, creates;
static int cpu_hyperv = -1;

bool
get_config_bool_default(const char *path, bool fallback)
{
	if (strcmp(path, "cpu.hyperv") == 0 && cpu_hyperv >= 0)
		return (cpu_hyperv != 0);
	return (fallback);
}

hv_return_t
test_get_max_ipa_size(uint32_t *bits)
{
	queries++;
	*bits = 40; /* Enough to cover the high RAM bank of a 126-GiB VM. */
	return (query_result);
}

hv_return_t
test_set_ipa_size(hv_vm_config_t config, uint32_t bits)
{
	assert(config == expected_config && bits == 40);
	sets++;
	return (set_result);
}

hv_return_t
test_vm_create(hv_vm_config_t config)
{
	assert(config == expected_config);
	assert(queries == 1 && sets == 1);
	creates++;
	return (create_result);
}

static void
check_create(hv_return_t query, hv_return_t set, hv_return_t create,
    hv_return_t expected, unsigned int expected_sets,
    unsigned int expected_creates)
{
	query_result = query;
	set_result = set;
	create_result = create;
	queries = sets = creates = 0;
	assert(vm_create_with_max_ipa(expected_config) == expected);
	assert(queries == 1 && sets == expected_sets);
	assert(creates == expected_creates);
}

int
main(void)
{
	expected_config = (hv_vm_config_t)(uintptr_t)1;
	check_create(HV_SUCCESS, HV_SUCCESS, HV_SUCCESS, HV_SUCCESS, 1, 1);
	check_create(HV_ERROR, HV_SUCCESS, HV_SUCCESS, HV_ERROR, 0, 0);
	check_create(HV_SUCCESS, HV_BAD_ARGUMENT, HV_SUCCESS,
	    HV_BAD_ARGUMENT, 1, 0);
	check_create(HV_SUCCESS, HV_SUCCESS, HV_NO_RESOURCES,
	    HV_NO_RESOURCES, 1, 1);
	for (cpu_hyperv = -1; cpu_hyperv <= 1; cpu_hyperv++) {
		assert(hyperv_config_enabled(false) == (cpu_hyperv != 0));
		assert(!hyperv_config_enabled(true));
	}
	puts("HVF maximum IPA configuration and error propagation passed");
	return (0);
}
