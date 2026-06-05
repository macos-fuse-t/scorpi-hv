/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <support/linker_set.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <scorpi/protocol/vhost_user.h>

#include "bhyverun.h"
#include "config.h"
#include "console.h"
#include "debug.h"
#include "pci_emul.h"
#include "pci_virtio_vhost.h"
#include "virtio.h"
#include "virtio_gpu.h"
#include "virtio_vhost_transport.h"
#include "vmmapi.h"

#define VHOST_DEFAULT_BACKEND_ID  "gpu0"
#define VHOST_DEFAULT_DEVICE_NAME SCORPI_VIRTIO_VHOST_DEVICE_GPU
#define VHOST_DEFAULT_QUEUE_SIZE  1024
#define VHOST_GPU_QUEUE_COUNT	  2
#define VHOST_GPU_CTRLQ		  0
#define VHOST_GPU_COLS_MAX	  7680
#define VHOST_GPU_ROWS_MAX	  4320
#define VHOST_GPU_COLS_DEFAULT	  1024
#define VHOST_GPU_ROWS_DEFAULT	  768
#define VHOST_GPU_COLS_MIN	  640
#define VHOST_GPU_ROWS_MIN	  480
#define VHOST_GPU_SUBDEVICE	  0x4690
#define VHOST_LEGACY_CTRL_BAR	  0
#define VHOST_LEGACY_FB_BAR	  1
#define VHOST_LEGACY_CTRL_SIZE	  512
#define VHOST_LEGACY_FB_SIZE	  (128 * 1024UL * 1024UL)
#define VHOST_LEGACY_RESOURCE_ID  0xFFFFFFFF
#define VHOST_GPU_TRANSPORT_FEATURES \
	(VIRTIO_RING_F_INDIRECT_DESC | VIRTIO_F_VERSION_1)
#define VHOST_GPU_DEVICE_FEATURES (1ULL << VIRTIO_GPU_F_EDID)

struct pci_vhost_softc {
	struct virtio_softc vsc_vs;
	struct virtio_consts vsc_consts;
	pthread_mutex_t vsc_mtx;

	struct pci_vhost_state vhost;

	struct virtio_gpu_config gpu_config;
	uint32_t resx;
	uint32_t resy;
	uint32_t start_resx;
	uint32_t start_resy;
	bool hdpi_enabled;
	struct vqueue_info queues[VHOST_GPU_QUEUE_COUNT];

	bool legacy_fb_direct_mapped;
	bool renderer_scanout_active;
	uint64_t legacy_fb_direct_gpa;
	char legacy_fb_shm_name[PSHMNAMLEN];
	char renderer_scanout_shm_name[PSHMNAMLEN];
	int legacy_fb_shm_fd;
	uint8_t *legacy_fb_base;
	size_t legacy_fb_size;
	size_t renderer_scanout_shm_size;
};

extern uuid_t vm_uuid;

static void pci_vhost_reset(void *vsc);
static void pci_vhost_neg_features(void *vsc, uint64_t negotiated_features);
static int pci_vhost_cfgread(void *vsc, int offset, int size, uint32_t *retval);
static int pci_vhost_cfgwrite(void *vsc, int offset, int size, uint32_t value);
static void pci_vhost_queue_notify(void *vsc, struct vqueue_info *vq);
static uint64_t pci_vhost_barread(struct pci_devinst *pi, int baridx,
    uint64_t offset, int size);
static void pci_vhost_barwrite(struct pci_devinst *pi, int baridx,
    uint64_t offset, int size, uint64_t value);
static void pci_vhost_baraddr(struct pci_devinst *pi, int baridx, int enabled,
    uint64_t address);
static void pci_vhost_backend_event(void *opaque,
    const struct scorpi_vhost_msg *msg);
static void pci_vhost_apply_renderer_scanout(struct pci_vhost_softc *sc,
    const struct scorpi_vhost_scanout *scanout);
static void pci_vhost_apply_renderer_dirty_rect(
    const struct scorpi_vhost_dirty_rect *rect);
static void pci_vhost_blank_legacy_fb(struct pci_vhost_softc *sc);
static void pci_vhost_unmap_legacy_fb(struct pci_vhost_softc *sc);
static void pci_vhost_destroy_legacy_fb(struct pci_vhost_softc *sc);

static bool
pci_vhost_mode_supports_hdpi(uint32_t width, uint32_t height)
{
	return (width <= VHOST_GPU_COLS_MAX / 2 &&
	    height <= VHOST_GPU_ROWS_MAX / 2);
}

static int
pci_vhost_parse_bool(const char *name, const char *value, bool *out)
{
	if (value == NULL)
		return (0);
	if (!strcmp(value, "on") || !strcmp(value, "true") ||
	    !strcmp(value, "yes")) {
		*out = true;
		return (0);
	}
	if (!strcmp(value, "off") || !strcmp(value, "false") ||
	    !strcmp(value, "no")) {
		*out = false;
		return (0);
	}
	EPRINTLN("virtio-gpu-vhost: invalid %s value '%s'", name, value);
	return (-1);
}

static void
pci_vhost_set_effective_resolution(struct pci_vhost_softc *sc)
{
	sc->resx = sc->start_resx;
	sc->resy = sc->start_resy;
	if (sc->hdpi_enabled) {
		sc->resx *= 2;
		sc->resy *= 2;
	}
}

static struct virtio_consts vhost_consts = {
	.vc_name = "virtio-gpu-vhost",
	.vc_nvq = VHOST_GPU_QUEUE_COUNT,
	.vc_cfgsize = sizeof(struct virtio_gpu_config),
	.vc_reset = pci_vhost_reset,
	.vc_qnotify = pci_vhost_queue_notify,
	.vc_cfgread = pci_vhost_cfgread,
	.vc_cfgwrite = pci_vhost_cfgwrite,
	.vc_apply_features = pci_vhost_neg_features,
	.vc_hv_caps = VHOST_GPU_TRANSPORT_FEATURES,
};

static void
pci_vhost_device_features(void *opaque, uint64_t device_features)
{
	struct pci_vhost_softc *sc = opaque;

	pthread_mutex_lock(&sc->vsc_mtx);
	if (pci_vhost_features_negotiated(&sc->vhost)) {
		pthread_mutex_unlock(&sc->vsc_mtx);
		return;
	}
	sc->vsc_consts.vc_hv_caps = VHOST_GPU_TRANSPORT_FEATURES |
	    (device_features & VHOST_GPU_DEVICE_FEATURES);
	pthread_mutex_unlock(&sc->vsc_mtx);

	if ((device_features & VHOST_GPU_DEVICE_FEATURES) !=
	    VHOST_GPU_DEVICE_FEATURES) {
		EPRINTLN(
		    "virtio-gpu-vhost: backend is missing required device features 0x%llx",
		    (unsigned long long)(VHOST_GPU_DEVICE_FEATURES &
			~device_features));
	}
}

static void
pci_vhost_backend_event(void *opaque, const struct scorpi_vhost_msg *msg)
{
	struct pci_vhost_softc *sc = opaque;

	pthread_mutex_lock(&sc->vsc_mtx);
	switch (msg->header.request) {
	case SCORPI_VHOST_MSG_SCANOUT:
		pci_vhost_apply_renderer_scanout(sc, &msg->payload.scanout);
		break;
	case SCORPI_VHOST_MSG_DIRTY_RECT:
		if (sc->renderer_scanout_active)
			pci_vhost_apply_renderer_dirty_rect(
			    &msg->payload.dirty_rect);
		break;
	default:
		break;
	}
	pthread_mutex_unlock(&sc->vsc_mtx);
}

static void
pci_vhost_set_legacy_scanout(struct pci_vhost_softc *sc)
{
	uint32_t stride;

	if (sc->legacy_fb_base == NULL) {
		console_set_scanout(false, 0, 0, 0, 0, NULL, 0, false);
		return;
	}

	stride = roundup2(sc->resx * 4, 32);
	console_set_scanout(true, sc->resx, sc->resy, stride,
	    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, sc->legacy_fb_shm_name,
	    sc->legacy_fb_size, true);
	console_update_scanout_rect(0, 0, sc->resx, sc->resy);
}

static void
pci_vhost_blank_legacy_fb(struct pci_vhost_softc *sc)
{
	size_t bytes;
	uint32_t stride;

	if (sc->legacy_fb_base == NULL || sc->legacy_fb_size == 0)
		return;

	stride = roundup2(sc->resx * 4, 32);
	bytes = (size_t)stride * sc->resy;
	if (bytes > sc->legacy_fb_size)
		bytes = sc->legacy_fb_size;
	memset(sc->legacy_fb_base, 0, bytes);
}

static void
pci_vhost_apply_renderer_scanout(struct pci_vhost_softc *sc,
    const struct scorpi_vhost_scanout *scanout)
{
	uint32_t stride;
	uint32_t format;

	if ((scanout->flags & SCORPI_VHOST_SCANOUT_F_ENABLED) == 0 ||
	    scanout->width == 0 || scanout->height == 0) {
		sc->renderer_scanout_active = false;
		pci_vhost_blank_legacy_fb(sc);
		pci_vhost_set_legacy_scanout(sc);
		return;
	}
	if (scanout->shm_name[0] == '\0' || scanout->shm_size == 0)
		return;

	snprintf(sc->renderer_scanout_shm_name,
	    sizeof(sc->renderer_scanout_shm_name), "%s", scanout->shm_name);
	sc->renderer_scanout_shm_size = (size_t)scanout->shm_size;
	stride = scanout->stride != 0 ? scanout->stride : scanout->width * 4;
	format = scanout->format != 0 ? scanout->format :
					VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
	PRINTLN(
	    "virtio-gpu-vhost: renderer scanout shm=%s active=%ux%u backing=%ux%u stride=%u size=%zu",
	    sc->renderer_scanout_shm_name, scanout->width, scanout->height,
	    scanout->backing_width, scanout->backing_height, stride,
	    sc->renderer_scanout_shm_size);
	console_set_scanout(true, scanout->width, scanout->height, stride,
	    format, sc->renderer_scanout_shm_name,
	    sc->renderer_scanout_shm_size, true);
	console_update_scanout_rect(0, 0, scanout->width, scanout->height);
	sc->renderer_scanout_active = true;
}

static void
pci_vhost_apply_renderer_dirty_rect(const struct scorpi_vhost_dirty_rect *rect)
{
	if (rect->width == 0 || rect->height == 0)
		return;
	console_update_scanout_rect(rect->x, rect->y, rect->width,
	    rect->height);
}

static int
pci_vhost_create_legacy_fb(struct pci_vhost_softc *sc)
{
	uuid_string_t uuid;

	if (sc->legacy_fb_base != NULL)
		return (0);

	uuid_unparse(vm_uuid, uuid);
	uuid[20] = 0;
	snprintf(sc->legacy_fb_shm_name, sizeof(sc->legacy_fb_shm_name),
	    "/%s-%08x", uuid, VHOST_LEGACY_RESOURCE_ID);

	if (shm_unlink(sc->legacy_fb_shm_name) == -1 && errno != ENOENT) {
		EPRINTLN("virtio-gpu-vhost: shm_unlink stale %s: %s",
		    sc->legacy_fb_shm_name, strerror(errno));
		return (-1);
	}

	sc->legacy_fb_shm_fd = shm_open(sc->legacy_fb_shm_name,
	    O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
	if (sc->legacy_fb_shm_fd == -1) {
		EPRINTLN("virtio-gpu-vhost: shm_open %s: %s",
		    sc->legacy_fb_shm_name, strerror(errno));
		return (-1);
	}

	sc->legacy_fb_size = VHOST_LEGACY_FB_SIZE;
	if (ftruncate(sc->legacy_fb_shm_fd, sc->legacy_fb_size) == -1) {
		EPRINTLN("virtio-gpu-vhost: ftruncate %s: %s",
		    sc->legacy_fb_shm_name, strerror(errno));
		pci_vhost_destroy_legacy_fb(sc);
		return (-1);
	}

	sc->legacy_fb_base = mmap(NULL, sc->legacy_fb_size,
	    PROT_READ | PROT_WRITE, MAP_SHARED, sc->legacy_fb_shm_fd, 0);
	if (sc->legacy_fb_base == MAP_FAILED) {
		EPRINTLN("virtio-gpu-vhost: mmap %s: %s",
		    sc->legacy_fb_shm_name, strerror(errno));
		sc->legacy_fb_base = NULL;
		pci_vhost_destroy_legacy_fb(sc);
		return (-1);
	}

	return (0);
}

static void
pci_vhost_unmap_legacy_fb(struct pci_vhost_softc *sc)
{
	if (sc->legacy_fb_direct_mapped) {
		int error;

		error = vm_munmap_memseg(sc->vsc_vs.vs_pi->pi_vmctx,
		    sc->legacy_fb_direct_gpa, VHOST_LEGACY_FB_SIZE);
		if (error != 0) {
			EPRINTLN(
			    "virtio-gpu-vhost: failed to unmap legacy framebuffer "
			    "BAR at 0x%llx size 0x%lx: %s",
			    sc->legacy_fb_direct_gpa, VHOST_LEGACY_FB_SIZE,
			    strerror(error));
		}
		sc->legacy_fb_direct_mapped = false;
		sc->legacy_fb_direct_gpa = 0;
	}
}

static void
pci_vhost_destroy_legacy_fb(struct pci_vhost_softc *sc)
{
	pci_vhost_unmap_legacy_fb(sc);
	if (sc->legacy_fb_base != NULL) {
		munmap(sc->legacy_fb_base, sc->legacy_fb_size);
		sc->legacy_fb_base = NULL;
	}
	if (sc->legacy_fb_shm_fd >= 0) {
		close(sc->legacy_fb_shm_fd);
		sc->legacy_fb_shm_fd = -1;
	}
	if (sc->legacy_fb_shm_name[0] != '\0') {
		shm_unlink(sc->legacy_fb_shm_name);
		sc->legacy_fb_shm_name[0] = '\0';
	}
	sc->legacy_fb_size = 0;
}

static uint64_t
pci_vhost_legacy_ctrl_read(struct pci_vhost_softc *sc, uint64_t offset,
    int size)
{
	uint64_t value = 0;

	if (offset + size > VHOST_LEGACY_CTRL_SIZE)
		return (0);

	switch (offset) {
	case 0:
		value = VHOST_LEGACY_FB_SIZE;
		break;
	case 4:
		value = sc->resx;
		break;
	case 6:
		value = sc->resy;
		break;
	case 8:
		value = 32;
		break;
	default:
		break;
	}
	return (value);
}

static void
pci_vhost_legacy_ctrl_write(struct pci_vhost_softc *sc, uint64_t offset,
    int size, uint64_t value)
{
	if (offset + size > VHOST_LEGACY_CTRL_SIZE)
		return;

	switch (offset) {
	case 4:
		sc->resx = (uint32_t)value;
		if (!sc->renderer_scanout_active)
			pci_vhost_set_legacy_scanout(sc);
		break;
	case 6:
		sc->resy = (uint32_t)value;
		if (!sc->renderer_scanout_active)
			pci_vhost_set_legacy_scanout(sc);
		break;
	default:
		break;
	}
}

static uint64_t
pci_vhost_legacy_fb_read(struct pci_vhost_softc *sc, uint64_t offset, int size)
{
	uint64_t value = UINT64_MAX;

	if (sc->legacy_fb_base == NULL || offset + size > sc->legacy_fb_size ||
	    size > (int)sizeof(value))
		return (UINT64_MAX);

	value = 0;
	memcpy(&value, sc->legacy_fb_base + offset, size);
	return (value);
}

static void
pci_vhost_legacy_fb_write(struct pci_vhost_softc *sc, uint64_t offset, int size,
    uint64_t value)
{
	if (sc->legacy_fb_base == NULL || offset + size > sc->legacy_fb_size ||
	    size > (int)sizeof(value))
		return;

	memcpy(sc->legacy_fb_base + offset, &value, size);
}

static uint64_t
pci_vhost_barread(struct pci_devinst *pi, int baridx, uint64_t offset, int size)
{
	struct pci_vhost_softc *sc = pi->pi_arg;

	switch (baridx) {
	case VHOST_LEGACY_CTRL_BAR:
		return (pci_vhost_legacy_ctrl_read(sc, offset, size));
	case VHOST_LEGACY_FB_BAR:
		return (pci_vhost_legacy_fb_read(sc, offset, size));
	default:
		return (vi_pci_read_modern(pi, baridx, offset, size));
	}
}

static void
pci_vhost_barwrite(struct pci_devinst *pi, int baridx, uint64_t offset,
    int size, uint64_t value)
{
	struct pci_vhost_softc *sc = pi->pi_arg;

	switch (baridx) {
	case VHOST_LEGACY_CTRL_BAR:
		pci_vhost_legacy_ctrl_write(sc, offset, size, value);
		break;
	case VHOST_LEGACY_FB_BAR:
		pci_vhost_legacy_fb_write(sc, offset, size, value);
		break;
	default:
		vi_pci_write_modern(pi, baridx, offset, size, value);
		break;
	}
}

static void
pci_vhost_baraddr(struct pci_devinst *pi, int baridx, int enabled,
    uint64_t address)
{
	struct pci_vhost_softc *sc = pi->pi_arg;
	bool created;
	int error;
	int prot;

	if (baridx != VHOST_LEGACY_FB_BAR)
		return;

	if (!enabled) {
		pci_vhost_unmap_legacy_fb(sc);
		return;
	}

	created = sc->legacy_fb_base == NULL;
	if (pci_vhost_create_legacy_fb(sc) != 0) {
		EPRINTLN(
		    "virtio-gpu-vhost: failed to create legacy framebuffer");
		return;
	}

	if ((pi->pi_bar[baridx].flags & PCIBAR_F_DIRECT_MAPPED) != 0 &&
	    !sc->legacy_fb_direct_mapped) {
		prot = PROT_READ | PROT_WRITE | PROT_DONT_ALLOCATE;
		error = vm_setup_memory_segment(pi->pi_vmctx, address,
		    VHOST_LEGACY_FB_SIZE, prot,
		    (uintptr_t *)&sc->legacy_fb_base);
		if (error != 0) {
			EPRINTLN(
			    "virtio-gpu-vhost: failed to direct-map legacy "
			    "framebuffer BAR at 0x%llx size 0x%lx: %s",
			    address, VHOST_LEGACY_FB_SIZE, strerror(error));
			pci_emul_set_bar_direct_mapped(pi, baridx, 0);
		} else {
			sc->legacy_fb_direct_mapped = true;
			sc->legacy_fb_direct_gpa = address;
		}
	}

	if (created)
		memset(sc->legacy_fb_base, 0, sc->legacy_fb_size);
	pci_vhost_set_legacy_scanout(sc);
}

static void
pci_vhost_gpu_transport_info(struct pci_vhost_softc *sc,
    struct pci_vhost_transport_info *info)
{
	memset(info, 0, sizeof(*info));
	info->ready_queue = VHOST_GPU_CTRLQ;
}

static void
pci_vhost_bind_callbacks(struct pci_vhost_softc *sc)
{
	struct pci_vhost_transport_info info;

	pci_vhost_gpu_transport_info(sc, &info);
	(void)pci_vhost_bind_transport(&sc->vhost, sc->vsc_vs.vs_pi->pi_vmctx,
	    &info);
}

static void
pci_vhost_reset(void *vsc)
{
	struct pci_vhost_softc *sc = vsc;

	pci_vhost_clear_features(&sc->vhost);
	pci_vhost_advance_reset_generation(&sc->vhost);
	sc->renderer_scanout_active = false;
	pci_vhost_blank_legacy_fb(sc);
	pci_vhost_set_legacy_scanout(sc);
	vi_reset_dev(&sc->vsc_vs);
	pci_vhost_bind_callbacks(sc);
}

static void
pci_vhost_neg_features(void *vsc, uint64_t negotiated_features)
{
	struct pci_vhost_softc *sc = vsc;

	pci_vhost_set_features(&sc->vhost, negotiated_features);
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
	struct pci_vhost_transport_info info;

	pci_vhost_gpu_transport_info(sc, &info);
	(void)pci_vhost_notify_queue_kick(&sc->vhost,
	    sc->vsc_vs.vs_pi->pi_vmctx, &info, vq->vq_num);
}

static int
pci_vhost_init(struct pci_devinst *pi, nvlist_t *nvl)
{
	struct pci_vhost_softc *sc;
	const char *backend_id;
	const char *device_name;
	const char *socket_path;
	pthread_mutexattr_t attr;
	int err;
	const char *value;

	backend_id = get_config_value_node(nvl, "backend");
	if (backend_id == NULL)
		backend_id = VHOST_DEFAULT_BACKEND_ID;

	device_name = get_config_value_node(nvl, "backend_device");
	if (device_name == NULL)
		device_name = VHOST_DEFAULT_DEVICE_NAME;
	if (strcmp(device_name, SCORPI_VIRTIO_VHOST_DEVICE_GPU) != 0) {
		EPRINTLN("virtio-gpu-vhost: unsupported device '%s'",
		    device_name);
		return (-1);
	}
	socket_path = get_config_value_node(nvl, "socket");

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		return (-1);

	sc->vsc_consts = vhost_consts;
	sc->start_resx = VHOST_GPU_COLS_DEFAULT;
	sc->start_resy = VHOST_GPU_ROWS_DEFAULT;
	sc->gpu_config.num_scanouts = 1;
	sc->gpu_config.num_capsets = 0;
	sc->legacy_fb_shm_fd = -1;

	value = get_config_value_node(nvl, "hdpi");
	if (pci_vhost_parse_bool("hdpi", value, &sc->hdpi_enabled) != 0) {
		free(sc);
		return (-1);
	}
	value = get_config_value_node(nvl, "w");
	if (value != NULL)
		sc->start_resx = strtol(value, NULL, 10);
	value = get_config_value_node(nvl, "h");
	if (value != NULL)
		sc->start_resy = strtol(value, NULL, 10);
	if (sc->start_resx > VHOST_GPU_COLS_MAX ||
	    sc->start_resy > VHOST_GPU_ROWS_MAX) {
		EPRINTLN("virtio-gpu-vhost: max resolution is %ux%u",
		    VHOST_GPU_COLS_MAX, VHOST_GPU_ROWS_MAX);
		free(sc);
		return (-1);
	}
	if (sc->start_resx < VHOST_GPU_COLS_MIN ||
	    sc->start_resy < VHOST_GPU_ROWS_MIN) {
		EPRINTLN("virtio-gpu-vhost: minimum resolution is %ux%u",
		    VHOST_GPU_COLS_MIN, VHOST_GPU_ROWS_MIN);
		free(sc);
		return (-1);
	}
	if (sc->hdpi_enabled &&
	    !pci_vhost_mode_supports_hdpi(sc->start_resx, sc->start_resy)) {
		EPRINTLN(
		    "virtio-gpu-vhost: hdpi resolution %ux%u exceeds max %ux%u",
		    sc->start_resx * 2, sc->start_resy * 2, VHOST_GPU_COLS_MAX,
		    VHOST_GPU_ROWS_MAX);
		free(sc);
		return (-1);
	}
	pci_vhost_set_effective_resolution(sc);

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sc->vsc_mtx, &attr);
	pthread_mutexattr_destroy(&attr);

	for (uint32_t i = 0; i < VHOST_GPU_QUEUE_COUNT; i++) {
		sc->queues[i].vq_qsize = VHOST_DEFAULT_QUEUE_SIZE;
		sc->queues[i].vq_maxqsize = VHOST_DEFAULT_QUEUE_SIZE;
		sc->queues[i].vq_notify = pci_vhost_queue_notify;
	}
	pci_vhost_state_init(&sc->vhost, &sc->vsc_vs, sc->queues,
	    VHOST_GPU_QUEUE_COUNT, backend_id, device_name);
	pci_vhost_set_device_features_cb(&sc->vhost, pci_vhost_device_features,
	    sc);
	pci_vhost_set_event_cb(&sc->vhost, pci_vhost_backend_event, sc);

	pci_set_cfgdata16(pi, PCIR_DEVICE, VIRTIO_DEV_GPU);
	pci_set_cfgdata16(pi, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_DISPLAY);
	pci_set_cfgdata8(pi, PCIR_SUBCLASS, PCIS_DISPLAY_OTHER);
	pci_set_cfgdata16(pi, PCIR_SUBDEV_0, VHOST_GPU_SUBDEVICE);
	pci_set_cfgdata16(pi, PCIR_SUBVEND_0, 0xFB5D);
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

	err = pci_emul_alloc_bar(pi, VHOST_LEGACY_CTRL_BAR, PCIBAR_MEM32,
	    VHOST_LEGACY_CTRL_SIZE);
	assert(err == 0);
	err = pci_emul_alloc_bar(pi, VHOST_LEGACY_FB_BAR, PCIBAR_MEM32,
	    VHOST_LEGACY_FB_SIZE);
	assert(err == 0);
	pci_emul_set_bar_direct_mapped(pi, VHOST_LEGACY_FB_BAR, 1);

	pci_vhost_bind_callbacks(sc);
	if (socket_path != NULL &&
	    virtio_vhost_transport_set_backend_socket(sc->vhost.backend_id,
		socket_path) != 0) {
		EPRINTLN(
		    "virtio-gpu-vhost: failed to configure backend socket %s",
		    socket_path);
		return (-1);
	}
	console_set_hdpi(sc->hdpi_enabled);
	console_set_hardware_mouse(false);
	return (0);
}

static const struct pci_devemu pci_de_vhost = {
	.pe_emu = "virtio-gpu-vhost",
	.pe_init = pci_vhost_init,
	.pe_barwrite = pci_vhost_barwrite,
	.pe_barread = pci_vhost_barread,
	.pe_baraddr = pci_vhost_baraddr,
	.pe_reset = vi_pci_reset,
#ifdef BHYVE_SNAPSHOT
	.pe_snapshot = vi_pci_snapshot,
	.pe_pause = vi_pci_pause,
	.pe_resume = vi_pci_resume,
#endif
};
PCI_EMUL_SET(pci_de_vhost);
