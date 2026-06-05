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

static uint16_t
pci_vhost_queue_size_from_gpa(const struct vqueue_info *vq)
{
	uint64_t desc_gpa;
	uint64_t avail_gpa;
	uint64_t size;

	desc_gpa = pci_vhost_queue_desc_gpa(vq);
	avail_gpa = vq->vq_avail_gpa;
	if (desc_gpa == 0 || avail_gpa <= desc_gpa)
		return (0);

	size = (avail_gpa - desc_gpa) / sizeof(struct vring_desc);
	if ((avail_gpa - desc_gpa) % sizeof(struct vring_desc) != 0 ||
	    size == 0 || size > UINT16_MAX || !powerof2((uint32_t)size))
		return (0);

	return ((uint16_t)size);
}

static uint16_t
pci_vhost_queue_size(const struct vqueue_info *vq)
{
	uint16_t size;

	if (vq->vq_qsize != 0)
		return (vq->vq_qsize);
	size = pci_vhost_queue_size_from_gpa(vq);
	if (size != 0)
		return (size);
	return (vq->vq_maxqsize);
}

static uint64_t
pci_vhost_queue_avail_gpa(const struct vqueue_info *vq)
{
	uint64_t desc_gpa;
	uint16_t queue_size;

	if (vq->vq_avail_gpa != 0)
		return (vq->vq_avail_gpa);
	desc_gpa = pci_vhost_queue_desc_gpa(vq);
	if (desc_gpa == 0)
		return (0);
	queue_size = pci_vhost_queue_size(vq);
	return (desc_gpa + (uint64_t)queue_size * sizeof(struct vring_desc));
}

static uint64_t
pci_vhost_queue_used_gpa(const struct vqueue_info *vq)
{
	uint64_t avail_gpa;
	uint16_t queue_size;

	if (vq->vq_used_gpa != 0)
		return (vq->vq_used_gpa);
	avail_gpa = pci_vhost_queue_avail_gpa(vq);
	if (avail_gpa == 0)
		return (0);
	queue_size = pci_vhost_queue_size(vq);
	return (roundup2(avail_gpa +
		(uint64_t)(2 + queue_size + 1) * sizeof(uint16_t),
	    VRING_ALIGN));
}

static bool
pci_vhost_queue_ready(const struct vqueue_info *vq)
{
	return (vq_ring_ready((struct vqueue_info *)vq) != 0 &&
	    vq->vq_enabled && pci_vhost_queue_size(vq) != 0);
}

static void
pci_vhost_interrupt(void *opaque, uint32_t queue_index)
{
	struct pci_vhost_state *state = opaque;
	struct vqueue_info *vq;

	if (state == NULL || state->vs == NULL || state->vs->vs_mtx == NULL)
		return;

	pthread_mutex_lock(state->vs->vs_mtx);
	if (queue_index < state->queue_count) {
		vq = &state->queues[queue_index];
		if (pci_vhost_queue_ready(vq))
			vq_endchains(vq, 1);
	}
	pthread_mutex_unlock(state->vs->vs_mtx);
}

void
pci_vhost_state_init(struct pci_vhost_state *state, struct virtio_softc *vs,
    struct vqueue_info *queues, uint32_t queue_count, const char *backend_id,
    const char *device_name)
{
	memset(state, 0, sizeof(*state));
	state->vs = vs;
	state->queues = queues;
	state->queue_count = queue_count;
	snprintf(state->backend_id, sizeof(state->backend_id), "%s",
	    backend_id);
	snprintf(state->device_name, sizeof(state->device_name), "%s",
	    device_name);
}

void
pci_vhost_set_features(struct pci_vhost_state *state, uint64_t features)
{
	state->features = features;
	state->features_negotiated = true;
}

void
pci_vhost_clear_features(struct pci_vhost_state *state)
{
	state->features = 0;
	state->features_negotiated = false;
}

bool
pci_vhost_features_negotiated(const struct pci_vhost_state *state)
{
	return (state->features_negotiated);
}

void
pci_vhost_set_device_features_cb(struct pci_vhost_state *state,
    pci_vhost_device_features_cb device_features_cb, void *opaque)
{
	state->device_features_cb = device_features_cb;
	state->device_features_opaque = opaque;
}

void
pci_vhost_set_event_cb(struct pci_vhost_state *state,
    pci_vhost_event_cb event_cb, void *opaque)
{
	state->event_cb = event_cb;
	state->event_opaque = opaque;
}

static void
pci_vhost_device_features(void *opaque, uint64_t device_features)
{
	struct pci_vhost_state *state = opaque;

	if (state == NULL || state->device_features_cb == NULL)
		return;
	state->device_features_cb(state->device_features_opaque,
	    device_features);
}

static void
pci_vhost_event(void *opaque, const struct scorpi_vhost_msg *msg)
{
	struct pci_vhost_state *state = opaque;

	if (state == NULL || state->event_cb == NULL)
		return;
	state->event_cb(state->event_opaque, msg);
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
		transport.queues[i].size = pci_vhost_queue_size(vq);
		transport.queues[i].desc_addr = pci_vhost_queue_desc_gpa(vq);
		transport.queues[i].avail_addr = pci_vhost_queue_avail_gpa(vq);
		transport.queues[i].used_addr = pci_vhost_queue_used_gpa(vq);
		transport.queues[i].ready = pci_vhost_queue_ready(vq);
	}
	vhost_vmmem_fill_transport(ctx, &transport);
	if (info != NULL && info->ready_queue < transport.queue_count)
		transport.ready = transport.queues[info->ready_queue].ready;

	return (virtio_vhost_transport_bind_device(state->backend_id,
	    &transport, pci_vhost_interrupt,
	    state->device_features_cb == NULL ? NULL :
						pci_vhost_device_features,
	    state->event_cb == NULL ? NULL : pci_vhost_event, state));
}

int
pci_vhost_notify_queue_kick(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info, uint32_t queue_index)
{
	(void)pci_vhost_bind_transport(state, ctx, info);
	return (virtio_vhost_transport_notify_queue_kick(state->backend_id,
	    queue_index));
}
