/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <stdint.h>
#include <stdio.h>
#include <uuid/uuid.h>

#include "vhost_vmmem.h"
#include "vmmapi.h"

extern uuid_t vm_uuid;

void
vhost_vmmem_shm_name(char *buf, size_t len, const char *suffix)
{
	uint64_t hash;

	hash = 1469598103934665603ULL;
	for (size_t i = 0; i < sizeof(vm_uuid); i++) {
		hash ^= vm_uuid[i];
		hash *= 1099511628211ULL;
	}
	snprintf(buf, len, "/svm-%016llx-%s", (unsigned long long)hash, suffix);
}

static void
vhost_vmmem_add_region(
    struct scorpi_virtio_vhost_transport_desc *transport,
    uint64_t guest_phys_addr, uint64_t size, const char *suffix, int fd)
{
	struct scorpi_virtio_vhost_memory_region_desc *region;

	if (size == 0 ||
	    transport->memory_region_count >=
		SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS)
		return;

	region = &transport->memory_regions[transport->memory_region_count++];
	region->index = transport->memory_region_count - 1;
	region->guest_phys_addr = guest_phys_addr;
	region->size = size;
	region->shm_fd = fd;
	region->shm_offset = 0;
	vhost_vmmem_shm_name(region->shm_name, sizeof(region->shm_name),
	    suffix);
}

void
vhost_vmmem_fill_transport(struct vmctx *ctx,
    struct scorpi_virtio_vhost_transport_desc *transport)
{
	char suffix[SCORPI_VIRTIO_VHOST_NAME_MAX];
	vm_paddr_t gpa;
	size_t size;
	int fd;

	for (unsigned int i = 0;
	     vm_get_external_memory_region(ctx, i, &gpa, &size, suffix,
		 sizeof(suffix), &fd) == 0;
	     i++) {
		vhost_vmmem_add_region(transport, gpa, size, suffix, fd);
	}
}
