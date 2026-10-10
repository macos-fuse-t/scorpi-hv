/* SPDX-License-Identifier: BSD-2-Clause */
#include <Hypervisor/Hypervisor.h>
#include <assert.h>
#include <libfdt.h>
#include <stdio.h>
#include <string.h>
#include <vmmapi.h>
#include <arch/arm64/memory_layout.h>
#include "../src/libvmm/hvf/internal.h"
#include "../src/arch/arm64/fdt.h"

const char *get_config_value(const char *path)
{
	(void)path;
	return (NULL);
}

bool get_config_bool_default(const char *path, bool fallback)
{
	(void)path;
	return (fallback);
}

static void
check_layout(size_t total, size_t expected_low)
{
	struct vmctx ctx = {0};
	char fdt[65536];
	int node, len, count = 0;
	uint64_t described = 0;
	size_t low = arm64_lowmem_size(total);

	assert(low == expected_low);
	ctx.memsegs[VM_MEMSEG_LOW].base = vm_get_lowmem_base(&ctx);
	ctx.memsegs[VM_MEMSEG_LOW].size = low;
	ctx.memsegs[VM_MEMSEG_LOW].addr = UINT64_C(0x1000000000);
	ctx.memsegs[VM_MEMSEG_HIGH].base = vm_get_highmem_base(&ctx);
	ctx.memsegs[VM_MEMSEG_HIGH].size = total - low;
	ctx.memsegs[VM_MEMSEG_HIGH].addr = UINT64_C(0x2000000000);
	assert(vm_get_lowmem_base(&ctx) == UINT64_C(0x40000000));
	assert(vm_get_highmem_base(&ctx) == UINT64_C(0x100000000));
	assert(vm_get_lowmem_size(&ctx) + vm_get_highmem_size(&ctx) == total);

	for (int i = 0; i < VM_MEMSEG_COUNT; i++) {
		uint64_t base = ctx.memsegs[i].base;
		size_t size = ctx.memsegs[i].size;
		uintptr_t host = ctx.memsegs[i].addr;

		if (size == 0) {
			assert(vm_map_gpa(&ctx, base, 1) == NULL);
			continue;
		}
		assert((uintptr_t)vm_map_gpa(&ctx, base, size) == host);
		assert((uintptr_t)vm_map_gpa(&ctx, base + size - 1, 1) == host + size - 1);
		assert(vm_map_gpa(&ctx, base - 1, 1) == NULL);
		assert(vm_map_gpa(&ctx, base + size, 1) == NULL);
		assert(vm_map_gpa(&ctx, base + size - 1, 2) == NULL);
		assert(vm_map_gpa(&ctx, base, SIZE_MAX) == NULL);
	}
	assert(vm_map_gpa(&ctx, UINT64_MAX, 2) == NULL);
	assert(vm_map_gpa(&ctx, 0x10000, 1) == NULL); /* UART */
	assert(vm_map_gpa(&ctx, 0x2f000000, 1) == NULL); /* GIC */
	assert(vm_map_gpa(&ctx, 0xa0000000, 1) == NULL); /* PCI BARs */
	assert(vm_map_gpa(&ctx, 0xffff0000, 1) == NULL); /* FDT */

	assert(fdt_init(&ctx, 4, fdt, sizeof(fdt)) == 0);
	fdt_finalize();
	assert(fdt_check_header(fdt) == 0);
	fdt_for_each_subnode(node, fdt, 0) {
		const char *type = fdt_getprop(fdt, node, "device_type", &len);
		if (type == NULL || strcmp(type, "memory") != 0)
			continue;
		const fdt64_t *reg = fdt_getprop(fdt, node, "reg", &len);
		assert(reg != NULL && len == 16); /* EDK2's parser contract */
		uint64_t base, size;
		memcpy(&base, reg, sizeof(base));
		memcpy(&size, reg + 1, sizeof(size));
		base = fdt64_to_cpu(base);
		size = fdt64_to_cpu(size);
		if (count == 0) {
			assert(base == UINT64_C(0x100000000));
			assert(size == total - low);
		} else {
			assert(count == 1 && low != 0);
			assert(base == UINT64_C(0x40000000) && size == low);
		}
		described += size;
		count++;
	}
	assert(count == (low != 0 ? 2 : 1));
	assert(described == total);
}

int main(void)
{
	const size_t mib = 1024 * 1024;
	check_layout(8192 * mib, 512 * mib);
	check_layout(12288 * mib, 512 * mib);
	check_layout(126 * 1024 * mib, 512 * mib);
	check_layout(128 * 1024 * mib, 512 * mib);
	check_layout(576 * mib, 512 * mib);
	check_layout(256 * mib, 192 * mib);
	check_layout(64 * mib, 0);
	puts("HVF low-RAM layout, GPA boundaries, and FDT tests passed");
	return (0);
}
