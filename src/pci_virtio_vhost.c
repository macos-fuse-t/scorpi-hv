/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pci_emul.h"
#include "pci_virtio_vhost.h"
#include "vhost_vmmem.h"
#include "virtio.h"
#include "virtio_vhost_transport.h"

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
	return (desc_gpa + (uint64_t)vq->vq_qsize * sizeof(struct vring_desc));
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
	return (roundup2(avail_gpa +
		(uint64_t)(2 + vq->vq_qsize + 1) * sizeof(uint16_t),
	    VRING_ALIGN));
}

static void
pci_vhost_interrupt(void *opaque, uint32_t queue_index)
{
	struct pci_vhost_state *state = opaque;

	if (queue_index >= state->queue_count)
		return;
	vq_endchains(&state->queues[queue_index], 1);
}

static void
pci_vhost_reset(void *opaque)
{
	struct pci_vhost_state *state = opaque;

	if (state->reset_cb != NULL)
		state->reset_cb(state->reset_opaque);
}

void
pci_vhost_state_init(struct pci_vhost_state *state, struct virtio_softc *vs,
    struct vqueue_info *queues, uint32_t queue_count, const char *backend_id,
    const char *device_name, pci_vhost_reset_cb reset_cb, void *reset_opaque)
{
	memset(state, 0, sizeof(*state));
	state->vs = vs;
	state->queues = queues;
	state->queue_count = queue_count;
	state->reset_cb = reset_cb;
	state->reset_opaque = reset_opaque;
	snprintf(state->backend_id, sizeof(state->backend_id), "%s",
	    backend_id);
	snprintf(state->device_name, sizeof(state->device_name), "%s",
	    device_name);
}

void
pci_vhost_set_features(struct pci_vhost_state *state, uint64_t features)
{
	state->features = features;
}

void
pci_vhost_set_backend_features_cb(struct pci_vhost_state *state,
    pci_vhost_backend_features_cb backend_features_cb)
{
	state->backend_features_cb = backend_features_cb;
}

void
pci_vhost_advance_reset_generation(struct pci_vhost_state *state)
{
	state->reset_generation++;
}

int
pci_vhost_bind_transport(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info)
{
	struct scorpi_virtio_vhost_transport_desc transport;

	memset(&transport, 0, sizeof(transport));
	snprintf(transport.backend_id, sizeof(transport.backend_id), "%s",
	    state->backend_id);
	snprintf(transport.device_name, sizeof(transport.device_name), "%s",
	    state->device_name);
	transport.features = state->features;
	transport.reset_generation = state->reset_generation;
	transport.queue_count = state->queue_count;
	for (uint32_t i = 0; i < transport.queue_count; i++) {
		struct vqueue_info *vq = &state->queues[i];

		transport.queues[i].index = vq->vq_num;
		transport.queues[i].size = vq->vq_qsize;
		transport.queues[i].desc_addr = pci_vhost_queue_desc_gpa(vq);
		transport.queues[i].avail_addr = pci_vhost_queue_avail_gpa(vq);
		transport.queues[i].used_addr = pci_vhost_queue_used_gpa(vq);
		transport.queues[i].ready = vq_ring_ready(vq) != 0;
	}
	vhost_vmmem_fill_transport(ctx, &transport);
	if (info != NULL && info->ready_queue < transport.queue_count)
		transport.ready = transport.queues[info->ready_queue].ready;

	return (virtio_vhost_transport_bind_device(state->backend_id,
	    &transport, pci_vhost_interrupt, pci_vhost_reset,
	    state->backend_features_cb, state));
}

int
pci_vhost_notify_queue_kick(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info, uint32_t queue_index)
{
	(void)pci_vhost_bind_transport(state, ctx, info);
	return (virtio_vhost_transport_notify_queue_kick(state->backend_id,
	    queue_index));
}
