/* SPDX-License-Identifier: BSD-2-Clause */
#define hv_vm_map test_vm_map
#define hv_vm_unmap test_vm_unmap
#include "../src/libvmm/hvf/vmmapi.c"
#include "../src/vhost_vmmem.c"

int raw_stdio;
uuid_t vm_uuid;
static unsigned int maps, unmaps;
static bool fail_map;
static char backing;

hv_return_t
test_vm_map(void *addr, hv_ipa_t gpa, size_t size, hv_memory_flags_t flags)
{
	(void)gpa;
	assert(addr == &backing && size != 0);
	assert(flags == (HV_MEMORY_READ | HV_MEMORY_WRITE));
	if (fail_map)
		return (HV_ERROR);
	maps++;
	return (HV_SUCCESS);
}

hv_return_t
test_vm_unmap(hv_ipa_t gpa, size_t size)
{
	(void)gpa;
	assert(size != 0);
	unmaps++;
	return (HV_SUCCESS);
}

static int
map_range(struct vmctx *ctx, uint64_t gpa, size_t size)
{
	uintptr_t addr = (uintptr_t)&backing;

	/* Mock HVF to exercise real bookkeeping without allocating guest RAM. */
	return (vm_setup_memory_segment(ctx, gpa, size,
	    PROT_READ | PROT_WRITE | PROT_DONT_ALLOCATE, &addr));
}

static void
map_ram_range(struct vmctx *ctx, uint64_t gpa, size_t size)
{
	struct mem_range *seg;

	assert(map_range(ctx, gpa, size) == 0);
	seg = &ctx->mem_ranges[ctx->num_mem_ranges - 1];
	snprintf(seg->shm_suffix, sizeof(seg->shm_suffix), "ram%d",
	    ctx->num_mem_ranges - 1);
	seg->shm_fd = 100 + ctx->num_mem_ranges - 1;
}

static void
check_ram_transport(size_t total, size_t expected_chunk,
    unsigned int expected_regions)
{
	struct vmctx ctx = {0};
	struct scorpi_virtio_vhost_transport_desc transport = {0};
	size_t low = arm64_lowmem_size(total);
	size_t high = total - low;
	size_t chunk = vm_shared_memory_chunk_size(high);
	uint64_t described = 0;
	uint64_t next = ARM64_HIGHMEM_BASE;

	assert(chunk == expected_chunk);
	for (size_t offset = 0; offset < high; offset += chunk)
		map_ram_range(&ctx, ARM64_HIGHMEM_BASE + offset,
		    MIN(chunk, high - offset));
	if (low != 0)
		map_ram_range(&ctx, ARM64_LOWMEM_BASE, low);
	vhost_vmmem_fill_transport(&ctx, &transport);
	assert(transport.memory_region_count == expected_regions);
	assert(transport.memory_region_count == (unsigned)ctx.num_mem_ranges);
	assert(expected_regions <= SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS);
	for (unsigned i = 0; i < expected_regions; i++) {
		struct scorpi_virtio_vhost_memory_region_desc *r =
		    &transport.memory_regions[i];
		assert(r->index == i && r->shm_fd == 100 + (int)i);
		assert(r->shm_offset == 0 && r->size <= chunk);
		if (low != 0 && i == expected_regions - 1) {
			assert(next == ARM64_HIGHMEM_BASE + high);
			assert(r->guest_phys_addr == ARM64_LOWMEM_BASE);
			assert(r->size == low);
		} else {
			assert(r->guest_phys_addr == next);
			next += r->size;
		}
		described += r->size;
	}
	assert(described == total);
}

int
main(void)
{
	struct vmctx ctx = {0};
	const size_t total = 12 * GB;
	const size_t low = arm64_lowmem_size(total);
	const size_t high = total - low;
	const size_t chunk = vm_shared_memory_chunk_size(high);
	const uint64_t fb = UINT64_C(0xa0000000);
	const size_t fb_size = UINT64_C(0x8000000);
	int count;

	for (size_t offset = 0; offset < high;
	     offset += chunk)
		assert(map_range(&ctx, ARM64_HIGHMEM_BASE + offset,
		    MIN(chunk, high - offset)) == 0);
	assert(map_range(&ctx, ARM64_LOWMEM_BASE, low) == 0);
	/* Three platform mappings fill the old 16-entry table. */
	assert(map_range(&ctx, 0, PAGE_SIZE) == 0);
	assert(map_range(&ctx, PAGE_SIZE, PAGE_SIZE) == 0);
	assert(map_range(&ctx, 2 * PAGE_SIZE, PAGE_SIZE) == 0);
	assert(ctx.num_mem_ranges == 16);
	assert(map_range(&ctx, fb, fb_size) == 0);
	assert(ctx.num_mem_ranges == 17 && maps == 17);
	assert(vm_mem_allocated(&ctx, fb));
	assert(vm_mem_allocated(&ctx, fb + fb_size - 1));
	assert(!vm_mem_allocated(&ctx, fb + fb_size));

	/* Later devices have room, and a full table still allows BAR relocation. */
	for (int i = 3; ctx.num_mem_ranges < VM_MAX_MEMORY_SEGMENTS; i++)
		assert(map_range(&ctx, i * PAGE_SIZE, PAGE_SIZE) == 0);
	count = ctx.num_mem_ranges;
	assert(map_range(&ctx, UINT64_C(0x10000000), PAGE_SIZE) == E2BIG);
	assert(vm_mem_allocated(&ctx, ARM64_HIGHMEM_BASE + high - 1));

	/* BAR relocation reuses its slot, including after a failed HVF map. */
	assert(vm_munmap_memseg(&ctx, fb, fb_size) == 0);
	assert(!vm_mem_allocated(&ctx, fb) && unmaps == 1);
	fail_map = true;
	assert(map_range(&ctx, fb, fb_size) == ENOMEM);
	assert(!vm_mem_allocated(&ctx, fb));
	fail_map = false;
	assert(map_range(&ctx, UINT64_C(0xb0000000), fb_size) == 0);
	assert(ctx.num_mem_ranges == count);
	assert(vm_mem_allocated(&ctx, UINT64_C(0xb0000000)));
	assert(vm_munmap_memseg(&ctx, UINT64_C(0xb0000000), fb_size) == 0);
	assert(unmaps == 2);

	check_ram_transport(64 * MB, GB, 1);
	check_ram_transport(12 * GB, GB, 13);
	check_ram_transport(15 * GB + ARM64_LOWMEM_SIZE, GB, 16);
	check_ram_transport(15 * GB + ARM64_LOWMEM_SIZE + PAGE_SIZE,
	    2 * GB, 9);
	check_ram_transport(126 * GB, 16 * GB, 9);
	check_ram_transport(128 * GB, 16 * GB, 9);
	check_ram_transport(256 * GB, 32 * GB, 9);
	assert(vm_shared_memory_chunk_size(SIZE_MAX) ==
	    (UINT64_C(1) << 61));
	puts("HVF 12-GiB framebuffer, large RAM transport, and slot reuse passed");
	return (0);
}
