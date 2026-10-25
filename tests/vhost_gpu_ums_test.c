/* SPDX-License-Identifier: BSD-2-Clause */
#include "pci_emul.h"
#undef PCI_EMUL_SET
#define PCI_EMUL_SET(x)
#include "../src/pci_virtio_gpu_vhost.c"
#include "../src/virtio.c"

uuid_t vm_uuid;
int raw_stdio;
static unsigned bindings, kicks;
static uint8_t ram[65536];

/* Queue operations must not update the display. */
void
console_set_scanout(bool active, int w, int h, int stride, uint32_t format,
    const char *name, size_t size, bool legacy)
{
	(void)active; (void)w; (void)h; (void)stride; (void)format;
	(void)name; (void)size; (void)legacy;
	assert(false);
}

void
console_update_scanout_rect(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
	assert(false);
}

bool
pci_vhost_features_negotiated(const struct pci_vhost_state *state)
{
	return (state->features_negotiated);
}

int
pci_vhost_bind_transport(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info)
{
	(void)ctx; (void)info;
	assert(state->queue_count == VHOST_GPU_QUEUE_CAPACITY);
	bindings++;
	return (0);
}

int
pci_vhost_notify_queue_kick(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info, uint32_t index)
{
	(void)ctx; (void)info;
	assert(index < state->queue_count && !state->queue_resetting[index]);
	kicks++;
	return (0);
}

void *
paddr_guest2host(struct vmctx *ctx, uintptr_t address, size_t size)
{
	(void)ctx;
	return (address <= sizeof(ram) && size <= sizeof(ram) - address ?
	    ram + address : NULL);
}

void
pci_lintr_deassert(struct pci_devinst *pi)
{
	(void)pi;
}

int
pci_msix_table_bar(struct pci_devinst *pi)
{
	(void)pi;
	return (3);
}

int
pci_msix_pba_bar(struct pci_devinst *pi)
{
	(void)pi;
	return (3);
}

int
pci_emul_msix_twrite(struct pci_devinst *pi, uint64_t offset, int size,
    uint64_t value)
{
	(void)pi; (void)offset; (void)size; (void)value;
	assert(false);
	return (0);
}

uint64_t
pci_emul_msix_tread(struct pci_devinst *pi, uint64_t offset, int size)
{
	(void)pi; (void)offset; (void)size;
	assert(false);
	return (0);
}

int
main(void)
{
	struct pci_devinst pi = {0};
	struct pci_vhost_softc sc = {0};
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sc.vsc_mtx, &attr);
	pthread_mutexattr_destroy(&attr);
	sc.vsc_consts.vc_nvq = VHOST_GPU_QUEUE_CAPACITY;
	sc.vhost.queue_count = VHOST_GPU_QUEUE_CAPACITY;
	sc.vsc_vs.vs_vc = &sc.vsc_consts;
	sc.vsc_vs.vs_pi = &pi;
	sc.vsc_vs.vs_queues = sc.queues;
	sc.vsc_vs.vs_mtx = &sc.vsc_mtx;
	sc.vsc_vs.vs_status = VIRTIO_CONFIG_STATUS_DRIVER_OK;
	pi.pi_arg = &sc;
	assert(pipe(sc.reset_fds) == 0);
	assert(fcntl(sc.reset_fds[0], F_SETFL, O_NONBLOCK) == 0);
	assert(fcntl(sc.reset_fds[1], F_SETFL, O_NONBLOCK) == 0);
	pci_vhost_device_features(&sc, 0);
	assert(sc.vhost.queue_count == 4 && sc.vsc_consts.vc_nvq == 4);
	pci_vhost_device_features(&sc, VHOST_GPU_DEVICE_FEATURES |
	    (1ULL << VIRTIO_GPU_F_SCORPI_USER_SUBMISSION) |
	    (1ULL << VHOST_GPU_F_RING_RESET));
	assert(sc.vhost.queue_count == 68 && sc.vsc_consts.vc_nvq == 68);
	/* Cursor capability follows configuration, independently of UMS. */
	const uint64_t cursor = 1ULL << VIRTIO_GPU_F_SCORPI_HOST_CURSOR;
	const uint64_t offered = VHOST_GPU_DEVICE_FEATURES | cursor |
	    (1ULL << VIRTIO_GPU_F_SCORPI_USER_SUBMISSION);
	pci_vhost_device_features(&sc, offered);
	assert(!(sc.vsc_consts.vc_hv_caps & cursor));
	sc.hardware_mouse_enabled = true;
	pci_vhost_device_features(&sc, offered);
	assert(sc.vsc_consts.vc_hv_caps & cursor);
	pci_vhost_device_features(&sc, offered & ~cursor);
	assert(!(sc.vsc_consts.vc_hv_caps & cursor));

	sc.vhost.features_negotiated = true;
	pci_vhost_device_features(&sc, 0);
	assert(sc.vhost.queue_count == 68); /* Stable after negotiation. */

	/* Extend only this frontend's common capability, preserving neighbours. */
	pci_set_cfgdata8(&pi, PCIR_CAP_PTR, 0x40);
	pci_set_cfgdata8(&pi, 0x40, PCIY_VENDOR);
	pci_set_cfgdata8(&pi, 0x41, 0x50);
	pci_set_cfgdata8(&pi, 0x43, VIRTIO_PCI_CAP_NOTIFY_CFG);
	pci_set_cfgdata8(&pi, 0x50, PCIY_VENDOR);
	pci_set_cfgdata8(&pi, 0x53, VIRTIO_PCI_CAP_COMMON_CFG);
	pci_set_cfgdata8(&pi, 0x54, VHOST_GPU_MODERN_BAR);
	pci_set_cfgdata32(&pi, 0x4c, 4);
	pci_set_cfgdata32(&pi, 0x5c, sizeof(struct virtio_pci_common_cfg));
	assert(pci_vhost_extend_common_cfg(&pi) == 0);
	assert(pci_get_cfgdata32(&pi, 0x5c) == 60);
	assert(pci_get_cfgdata32(&pi, 0x4c) == 4);
	assert(sizeof(struct virtio_pci_common_cfg) == 56);

#define WRITE(offset, size, value) pci_vhost_barwrite(&pi, VHOST_GPU_MODERN_BAR, offset, size, value)
#define READ(offset, size) pci_vhost_barread(&pi, VHOST_GPU_MODERN_BAR, offset, size)
	for (unsigned i = 0; i < VHOST_GPU_QUEUE_CAPACITY; i++) {
		struct vqueue_info *q = &sc.queues[i];
		q->vq_num = i;
		q->vq_maxqsize = 1024;
		q->vq_qsize = 8;
		q->vq_enabled = true;
		q->vq_flags = VQ_ALLOC;
		q->vq_desc_gpa = 0x1000;
		q->vq_avail_gpa = 0x2000;
		q->vq_used_gpa = 0x3000;
		WRITE(VIRTIO_PCI_COMMON_Q_SELECT, 2, i);
		assert(READ(VIRTIO_PCI_COMMON_Q_SELECT, 2) == i);
		WRITE(VIRTIO_PCI_COMMON_Q_ENABLE, 2, 0);
		WRITE(VIRTIO_PCI_COMMON_Q_DESCLO, 4, 0xbad);
		assert(q->vq_enabled && q->vq_desc_gpa == 0x1000);
		sc.vsc_vs.vs_negotiated_caps = 0;
		WRITE(VHOST_GPU_COMMON_Q_RESET, 2, 1);
		assert(!sc.vhost.queue_resetting[i]);
		sc.vsc_vs.vs_negotiated_caps = 1ULL << VHOST_GPU_F_RING_RESET;
		WRITE(VHOST_GPU_COMMON_Q_RESET, 4, 1);
		assert(!sc.vhost.queue_resetting[i]);
		WRITE(VHOST_GPU_COMMON_Q_RESET, 2, 1);
		assert(sc.vhost.queue_resetting[i] && q->vq_enabled);
		assert(sc.queue_resets[i].serial == i + 1 && bindings == i + 1);
		assert(READ(VHOST_GPU_COMMON_Q_RESET, 2) == 1);
		assert(READ(56, 4) == 0x10000);
		WRITE(VHOST_GPU_COMMON_Q_RESET, 2, 1);
		WRITE(VHOST_GPU_COMMON_Q_RESET, 2, 0);
		WRITE(VIRTIO_PCI_COMMON_Q_SIZE, 2, 16);
		assert(q->vq_qsize == 8 && bindings == i + 1);
		pci_vhost_queue_notify(&sc, q);
		assert(kicks == 0); /* No kernel or user kick while reset is pending. */
		pci_vhost_complete_queue_reset(&sc, i); /* Matching renderer ACK. */
		assert(!q->vq_enabled && !sc.vhost.queue_resetting[i]);
		assert(q->vq_qsize == 1024 && q->vq_msix_idx == VIRTIO_MSI_NO_VECTOR);
		assert(!q->vq_desc_gpa && !q->vq_avail_gpa && !q->vq_used_gpa);
		assert(READ(VHOST_GPU_COMMON_Q_RESET, 2) == 0);
		WRITE(VIRTIO_PCI_COMMON_Q_SIZE, 2, 8);
		WRITE(VIRTIO_PCI_COMMON_Q_DESCLO, 4, 0x1000);
		WRITE(VIRTIO_PCI_COMMON_Q_AVAILLO, 4, 0x2000);
		WRITE(VIRTIO_PCI_COMMON_Q_USEDLO, 4, 0x3000);
		WRITE(VIRTIO_PCI_COMMON_Q_ENABLE, 2, 1);
		assert(q->vq_enabled && vq_ring_ready(q));
		/* Direct UMD notifications retain the same per-page queue mapping. */
		if (i >= VHOST_GPU_QUEUE_COUNT) {
			const uint64_t offset = (i - VHOST_GPU_QUEUE_COUNT) * SCORPI_GPU_USER_NOTIFY_STRIDE;
			pci_vhost_barwrite(&pi, SCORPI_GPU_USER_NOTIFY_BAR, offset, 4, UINT32_MAX);
			assert(kicks == 0); /* UMS feature not negotiated. */
			sc.vsc_vs.vs_negotiated_caps |= 1ULL << VIRTIO_GPU_F_SCORPI_USER_SUBMISSION;
			pci_vhost_barwrite(&pi, SCORPI_GPU_USER_NOTIFY_BAR, offset + 4, 4, 0);
			assert(kicks == 0);
			pci_vhost_barwrite(&pi, SCORPI_GPU_USER_NOTIFY_BAR, offset, 4, UINT32_MAX);
			assert(kicks == 1);
			kicks = 0;
		}
	}
	WRITE(VIRTIO_PCI_COMMON_Q_SELECT, 2, VHOST_GPU_QUEUE_CAPACITY);
	WRITE(VHOST_GPU_COMMON_Q_RESET, 2, 1);
	assert(bindings == VHOST_GPU_QUEUE_CAPACITY && READ(VHOST_GPU_COMMON_Q_RESET, 2) == 0);
	close(sc.reset_fds[0]);
	close(sc.reset_fds[1]);
	pthread_mutex_destroy(&sc.vsc_mtx);
	puts("Vhost UMS: 68 queues, isolated kicks, reset negotiation, delayed completion and ring reuse passed");
	return (0);
}
