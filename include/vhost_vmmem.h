#pragma once

#include <stddef.h>

#include "virtio_vhost_transport.h"

struct vmctx;

void vhost_vmmem_shm_name(char *buf, size_t len, const char *suffix);
void vhost_vmmem_fill_transport(struct vmctx *ctx,
    struct scorpi_virtio_vhost_transport_desc *transport);
