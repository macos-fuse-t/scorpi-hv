#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <scorpi/protocol/virtio_vhost.h>

struct virtio_softc;
struct vqueue_info;
struct vmctx;

typedef void (*pci_vhost_reset_cb)(void *opaque);
typedef void (*pci_vhost_backend_features_cb)(void *opaque,
    uint64_t backend_features);

struct pci_vhost_state {
	struct virtio_softc *vs;
	struct vqueue_info *queues;
	uint32_t queue_count;
	char backend_id[SCORPI_VIRTIO_VHOST_NAME_MAX];
	char device_name[SCORPI_VIRTIO_VHOST_NAME_MAX];
	uint64_t features;
	uint32_t reset_generation;
	pci_vhost_reset_cb reset_cb;
	pci_vhost_backend_features_cb backend_features_cb;
	void *reset_opaque;
};

struct pci_vhost_transport_info {
	uint32_t ready_queue;
};

void pci_vhost_state_init(struct pci_vhost_state *state,
    struct virtio_softc *vs, struct vqueue_info *queues, uint32_t queue_count,
    const char *backend_id, const char *device_name, pci_vhost_reset_cb reset_cb,
    void *reset_opaque);
void pci_vhost_set_features(struct pci_vhost_state *state, uint64_t features);
void pci_vhost_set_backend_features_cb(struct pci_vhost_state *state,
    pci_vhost_backend_features_cb backend_features_cb);
void pci_vhost_advance_reset_generation(struct pci_vhost_state *state);
int pci_vhost_bind_transport(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info);
int pci_vhost_notify_queue_kick(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info, uint32_t queue_index);
