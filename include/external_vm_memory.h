#pragma once

#include <stddef.h>

#include "virtio_external_backend.h"

struct vmctx;

void external_vm_memory_shm_name(char *buf, size_t len, const char *suffix);
void external_vm_memory_fill_transport(struct vmctx *ctx,
    struct virtio_external_transport_desc *transport);
