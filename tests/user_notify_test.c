#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <scorpi/protocol/virtio_gpu_user.h>
#include <scorpi/protocol/vhost_user.h>

int main(void)
{
    _Static_assert(SCORPI_VHOST_MAX_QUEUES == 68, "4 kernel + 64 user queues");
    for (uint32_t slot = 0; slot < SCORPI_VIRTIO_GPU_USER_QUEUE_COUNT; ++slot) {
        uint64_t offset = (uint64_t)slot * SCORPI_GPU_USER_NOTIFY_STRIDE;
        uint32_t queue = UINT32_MAX;
        assert(scorpi_gpu_user_notify_queue(offset, 2, &queue));
        assert(queue == slot + 4);
        assert(scorpi_gpu_user_notify_queue(offset, 4, &queue));
        assert(queue == slot + 4);
        assert(!scorpi_gpu_user_notify_queue(offset, 1, &queue));
        assert(!scorpi_gpu_user_notify_queue(offset, 8, &queue));
        for (uint32_t byte = 1; byte < SCORPI_GPU_USER_NOTIFY_STRIDE; ++byte) {
            queue = UINT32_MAX;
            assert(!scorpi_gpu_user_notify_queue(offset + byte, 4, &queue));
            assert(queue == UINT32_MAX);
        }
    }
    uint32_t queue = UINT32_MAX;
    assert(!scorpi_gpu_user_notify_queue(SCORPI_GPU_USER_NOTIFY_BYTES, 4, &queue));
    assert(!scorpi_gpu_user_notify_queue(UINT64_MAX, 4, &queue));
    assert(!scorpi_gpu_user_notify_queue(0, 4, NULL));
    puts("user notify: all 64 pages isolated; invalid offsets and widths rejected");
}
