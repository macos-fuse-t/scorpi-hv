#pragma once

#include <stdbool.h>

void virtio_external_backend_init(void);
bool virtio_external_backend_registered(const char *backend_id);
