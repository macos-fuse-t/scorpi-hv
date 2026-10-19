#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "virtio_vhost.h"

/* Negotiate only with a backend implementing queue registration, ownership,
 * reset and WDDM completion. The aperture contains only
 * notifications; it never exposes the common virtio configuration page. */
#define VIRTIO_GPU_F_SCORPI_USER_SUBMISSION 12u
#define SCORPI_GPU_USER_NOTIFY_BAR 4u
#define SCORPI_GPU_USER_NOTIFY_STRIDE (64u * 1024u)
#define SCORPI_GPU_USER_NOTIFY_BYTES \
    (SCORPI_GPU_USER_NOTIFY_STRIDE * SCORPI_VIRTIO_GPU_USER_QUEUE_COUNT)

static inline bool
scorpi_gpu_user_notify_queue(uint64_t offset, unsigned size, uint32_t *queue)
{
    if (queue == 0 || (size != 2 && size != 4) ||
        offset >= SCORPI_GPU_USER_NOTIFY_BYTES ||
        offset % SCORPI_GPU_USER_NOTIFY_STRIDE != 0)
        return false;
    *queue = SCORPI_VIRTIO_GPU_KERNEL_QUEUE_COUNT +
        (uint32_t)(offset / SCORPI_GPU_USER_NOTIFY_STRIDE);
    return true;
}
