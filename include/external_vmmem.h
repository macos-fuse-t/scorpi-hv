#pragma once

#include <stddef.h>

#include "virtio_external_backend.h"

struct vmctx;

void external_vmmem_shm_name(char *buf, size_t len, const char *suffix);
void external_vmmem_fill_transport(struct vmctx *ctx,
    struct virtio_external_transport_desc *transport);
