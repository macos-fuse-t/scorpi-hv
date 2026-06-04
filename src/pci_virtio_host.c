/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>
#include <support/linker_set.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "debug.h"
#include "bhyverun.h"
#include "external_vmmem.h"
#include "pci_emul.h"
#include "virtio.h"
#include "virtio_external_backend.h"
#include "virtio_gpu.h"

#define VHOST_DEFAULT_BACKEND_ID "gpu0"
#define VHOST_DEFAULT_DEVICE_NAME "virtio-gpu"
#define VHOST_DEFAULT_QUEUE_SIZE 1024
#define VHOST_GPU_QUEUE_COUNT 2
#define VHOST_GPU_CTRLQ 0

struct pci_vhost_softc {
	struct virtio_softc vsc_vs;
	struct virtio_consts vsc_consts;
	pthread_mutex_t vsc_mtx;

	char backend_id[SCORPI_VIRTIO_EXTERNAL_NAME_MAX];
	char device_name[SCORPI_VIRTIO_EXTERNAL_NAME_MAX];
	uint64_t features;
	uint32_t reset_generation;

	struct virtio_gpu_config gpu_config;
	struct vqueue_info queues[VHOST_GPU_QUEUE_COUNT];
};

static void pci_vhost_reset(void *vsc);
static void pci_vhost_neg_features(void *vsc, uint64_t negotiated_features);
static int pci_vhost_cfgread(void *vsc, int offset, int size,
    uint32_t *retval);
static int pci_vhost_cfgwrite(void *vsc, int offset, int size, uint32_t value);
static void pci_vhost_queue_notify(void *vsc, struct vqueue_info *vq);

static struct virtio_consts vhost_consts = {
	.vc_name = "virtio-host",
	.vc_nvq = VHOST_GPU_QUEUE_COUNT,
	.vc_cfgsize = sizeof(struct virtio_gpu_config),
	.vc_reset = pci_vhost_reset,
	.vc_qnotify = pci_vhost_queue_notify,
	.vc_cfgread = pci_vhost_cfgread,
	.vc_cfgwrite = pci_vhost_cfgwrite,
	.vc_apply_features = pci_vhost_neg_features,
	.vc_hv_caps = VIRTIO_RING_F_INDIRECT_DESC | VIRTIO_F_VERSION_1 |
	    (1ULL << VIRTIO_GPU_F_EDID),
};

static uint64_t
pci_vhost_queue_desc_gpa(const struct vqueue_info *vq)
{
	if (vq->vq_desc_gpa != 0)
		return (vq->vq_desc_gpa);
	if (vq->vq_pfn != 0)
		return ((uint64_t)vq->vq_pfn << VRING_PFN);
	return (0);
}

static uint64_t
pci_vhost_queue_avail_gpa(const struct vqueue_info *vq)
{
	uint64_t desc_gpa;

	if (vq->vq_avail_gpa != 0)
		return (vq->vq_avail_gpa);
	desc_gpa = pci_vhost_queue_desc_gpa(vq);
	if (desc_gpa == 0)
		return (0);
	return (desc_gpa + (uint64_t)vq->vq_qsize *
	    sizeof(struct vring_desc));
}

static uint64_t
pci_vhost_queue_used_gpa(const struct vqueue_info *vq)
{
	uint64_t avail_gpa;

	if (vq->vq_used_gpa != 0)
		return (vq->vq_used_gpa);
	avail_gpa = pci_vhost_queue_avail_gpa(vq);
	if (avail_gpa == 0)
		return (0);
	return (roundup2(avail_gpa + (uint64_t)(2 + vq->vq_qsize + 1) *
	    sizeof(uint16_t), VRING_ALIGN));
}

static void
pci_vhost_interrupt(void *opaque, uint32_t queue_index)
{
	struct pci_vhost_softc *sc = opaque;

	if (queue_index >= (uint32_t)sc->vsc_consts.vc_nvq)
		return;
	vq_interrupt(&sc->vsc_vs, &sc->queues[queue_index]);
}

static void
pci_vhost_bind_callbacks(struct pci_vhost_softc *sc)
{
	struct scorpi_virtio_external_transport_desc transport;

	memset(&transport, 0, sizeof(transport));
	snprintf(transport.backend_id, sizeof(transport.backend_id), "%s",
	    sc->backend_id);
	snprintf(transport.device_name, sizeof(transport.device_name), "%s",
	    sc->device_name);
	transport.features = sc->features;
	transport.reset_generation = sc->reset_generation;
	transport.queue_count = sc->vsc_consts.vc_nvq;
	for (uint32_t i = 0; i < transport.queue_count; i++) {
		struct vqueue_info *vq = &sc->queues[i];

		transport.queues[i].index = vq->vq_num;
		transport.queues[i].size = vq->vq_qsize;
		transport.queues[i].desc_addr = pci_vhost_queue_desc_gpa(vq);
		transport.queues[i].avail_addr = pci_vhost_queue_avail_gpa(vq);
		transport.queues[i].used_addr = pci_vhost_queue_used_gpa(vq);
		transport.queues[i].ready = vq_ring_ready(vq) != 0;
	}
	external_vmmem_fill_transport(sc->vsc_vs.vs_pi->pi_vmctx,
	    &transport);
	transport.ready = transport.queues[VHOST_GPU_CTRLQ].ready;

	(void)virtio_external_backend_bind_device(sc->backend_id, &transport,
	    pci_vhost_interrupt, pci_vhost_reset, sc);
}

static void
pci_vhost_reset(void *vsc)
{
	struct pci_vhost_softc *sc = vsc;

	sc->reset_generation++;
	vi_reset_dev(&sc->vsc_vs);
	pci_vhost_bind_callbacks(sc);
}

static void
pci_vhost_neg_features(void *vsc, uint64_t negotiated_features)
{
	struct pci_vhost_softc *sc = vsc;

	sc->features = negotiated_features;
	pci_vhost_bind_callbacks(sc);
}

static int
pci_vhost_cfgread(void *vsc, int offset, int size, uint32_t *retval)
{
	struct pci_vhost_softc *sc = vsc;
	uint32_t value = 0;

	if (offset < 0 || size < 0 ||
	    (size_t)offset + (size_t)size > sizeof(sc->gpu_config))
		return (EINVAL);
	memcpy(&value, ((uint8_t *)&sc->gpu_config) + offset, size);
	*retval = value;
	return (0);
}

static int
pci_vhost_cfgwrite(void *vsc, int offset, int size, uint32_t value)
{
	struct pci_vhost_softc *sc = vsc;

	if (offset == 4 && size == 4) {
		sc->gpu_config.events_read &= ~value;
		return (0);
	}
	return (0);
}

static void
pci_vhost_queue_notify(void *vsc, struct vqueue_info *vq)
{
	struct pci_vhost_softc *sc = vsc;

	pci_vhost_bind_callbacks(sc);
	(void)virtio_external_backend_notify_queue_kick(sc->backend_id,
	    vq->vq_num);
}

static int
pci_vhost_init(struct pci_devinst *pi, nvlist_t *nvl)
{
	struct pci_vhost_softc *sc;
	const char *backend_id;
	const char *device_name;
	pthread_mutexattr_t attr;
	int err;

	backend_id = get_config_value_node(nvl, "backend");
	if (backend_id == NULL)
		backend_id = VHOST_DEFAULT_BACKEND_ID;

	device_name = get_config_value_node(nvl, "device");
	if (device_name == NULL)
		device_name = VHOST_DEFAULT_DEVICE_NAME;
	if (strcmp(device_name, "virtio-gpu") != 0) {
		EPRINTLN("virtio-host: unsupported device '%s'", device_name);
		return (-1);
	}

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		return (-1);

	snprintf(sc->backend_id, sizeof(sc->backend_id), "%s", backend_id);
	snprintf(sc->device_name, sizeof(sc->device_name), "%s", device_name);
	sc->vsc_consts = vhost_consts;
	sc->gpu_config.num_scanouts = 1;
	sc->gpu_config.num_capsets = 0;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sc->vsc_mtx, &attr);
	pthread_mutexattr_destroy(&attr);

	for (uint32_t i = 0; i < VHOST_GPU_QUEUE_COUNT; i++) {
		sc->queues[i].vq_qsize = VHOST_DEFAULT_QUEUE_SIZE;
		sc->queues[i].vq_notify = pci_vhost_queue_notify;
	}

	pci_set_cfgdata16(pi, PCIR_DEVICE, VIRTIO_DEV_GPU);
	pci_set_cfgdata16(pi, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_DISPLAY);
	pci_set_cfgdata8(pi, PCIR_SUBCLASS, PCIS_DISPLAY_OTHER);
	pci_set_cfgdata16(pi, PCIR_SUBDEV_0, 0x4680);
	pci_set_cfgdata16(pi, PCIR_SUBVEND_0, VIRTIO_VENDOR);
	pci_set_cfgdata8(pi, PCIR_REVID, 1);

	vi_softc_linkup(&sc->vsc_vs, &sc->vsc_consts, sc, pi, sc->queues);
	sc->vsc_vs.vs_mtx = &sc->vsc_mtx;

	if (vi_intr_init(&sc->vsc_vs, 3, msix_supported())) {
		free(sc);
		return (1);
	}

	vi_set_mmio_bar_modern(&sc->vsc_vs, 2);
	err = add_virtio10_pci_caps(&sc->vsc_vs, 2);
	if (err) {
		free(sc);
		return (err);
	}

	pci_vhost_bind_callbacks(sc);
	return (0);
}

static const struct pci_devemu pci_de_vhost = {
	.pe_emu = "virtio-host",
	.pe_init = pci_vhost_init,
	.pe_barwrite = vi_pci_write_modern,
	.pe_barread = vi_pci_read_modern,
	.pe_reset = vi_pci_reset,
#ifdef BHYVE_SNAPSHOT
	.pe_snapshot = vi_pci_snapshot,
	.pe_pause = vi_pci_pause,
	.pe_resume = vi_pci_resume,
#endif
};
PCI_EMUL_SET(pci_de_vhost);
