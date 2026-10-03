/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>
#include "vhost_vmmem.h"
#include "vmmapi.h"

uuid_t vm_uuid = {1, 2, 3};
static unsigned region_count;
static int region_fds[SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS];

int
vm_get_external_memory_region(struct vmctx *ctx, unsigned int index,
    vm_paddr_t *gpa, size_t *len, char *suffix, size_t suffix_len, int *fd)
{
	(void)ctx;
	if (index >= region_count)
		return (ENOENT);
	*gpa = UINT64_C(0x100000000) + index * UINT64_C(0x100000);
	*len = 0x100000;
	*fd = region_fds[index];
	snprintf(suffix, suffix_len, "region%u", index);
	return (0);
}

int
main(void)
{
	struct scorpi_virtio_vhost_transport_desc transport = {0};
	char expected[64];
	region_count = SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS;
	for (unsigned i = 0; i < region_count; ++i)
		region_fds[i] = (int)i; /* fd zero is valid, too. */
	region_fds[3] = -1;
	vhost_vmmem_fill_transport(NULL, &transport);
	assert(transport.memory_region_count == region_count);
	vhost_vmmem_shm_name(expected, sizeof(expected), "region3");
	for (unsigned i = 0; i < region_count; ++i) {
		struct scorpi_virtio_vhost_memory_region_desc *r = &transport.memory_regions[i];
		assert(r->index == i && r->shm_fd == region_fds[i]);
		assert(r->guest_phys_addr == UINT64_C(0x100000000) + i * UINT64_C(0x100000));
		assert(r->size == 0x100000 && r->shm_offset == 0);
		if (i == 3)
			assert(strcmp(r->shm_name, expected) == 0);
		else
			assert(r->shm_name[0] == '\0');
	}
	/* Reuse storage after the previously named region gains a live FD. */
	region_fds[3] = 31;
	transport.memory_region_count = 0;
	vhost_vmmem_fill_transport(NULL, &transport);
	assert(transport.memory_regions[3].shm_fd == 31);
	assert(transport.memory_regions[3].shm_name[0] == '\0');
	puts("vhost memory transport: live FDs, named fallback, maximum table and reuse PASS");
	return (0);
}
