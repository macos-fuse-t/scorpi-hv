/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <support/linker_set.h>
#include <uuid/uuid.h>
#include "common.h"
// #include <machine/vmm_snapshot.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif
#include "compat.h"

#include "bhyverun.h"
#include "cnc.h"
#include "config.h"
#include "console.h"
#include "debug.h"
#include "edid.h"
#include "iov.h"
#include "mevent.h"
#include "net_backends.h"
#include "net_utils.h"
#include "pci_emul.h"
#include "virtio.h"
#include "virtio_gpu.h"
#include "vmmapi.h"

static int vgpu_debug = 0;
#define DPRINTF(params) \
	if (vgpu_debug) \
	PRINTLN params
#define WPRINTF(params)	       PRINTLN params

#define VQ_MAX_DESC	       512

#define VGPU_MAXQ	       3
#define VGPU_RINGSZ	       1024
#define VGPU_EDID_DPI	       96
#define VGPU_EDID_MM_PER_INCH_X10 254

#define VGPU_CTRL	       0
#define VGPU_CURSOR	       1

#define VGPU_S_HOSTCAPS	       (VIRTIO_RING_F_INDIRECT_DESC)

#define COLS_MAX	       7680
#define ROWS_MAX	       4320

#define COLS_DEFAULT	       1024
#define ROWS_DEFAULT	       768

#define COLS_MIN	       640
#define ROWS_MIN	       480

#define LEGACY_FRAMEBUFFER_BAR 1
#define LEGACY_CTRL_BAR	       0
#define VGPU_LEGACY_RESOURCE_ID 0xFFFFFFFF

#define DMEMSZ		       512

#define FB_SIZE		       (128 * 1024UL * 1024UL)

struct vgpu_scanout {
	uint32_t resource_id;

	uint32_t width;
	uint32_t height;
	uint32_t stride;

	uint32_t format;

	char shm_name[PSHMNAMLEN];
	int shm_fd;
	char *base_ptr;
	size_t size;

	struct iovec *backing_iov;
	int iov_cnt;

	LIST_ENTRY(vgpu_scanout) entries;
};

struct pci_vgpu_softc {
	struct virtio_softc vsc_vs;
	struct vqueue_info vsc_vq;
	pthread_mutex_t vsc_mtx;

	uint64_t vsc_features; /* negotiated features */

	struct virtio_consts vsc_consts;
	struct virtio_gpu_config vsc_config;

	struct vqueue_info vsc_queues[VGPU_MAXQ - 1];

	uint32_t resx;
	uint32_t resy;
	uint32_t start_resx;
	uint32_t start_resy;
	uint32_t max_resx;
	uint32_t max_resy;
	uint32_t host_scale;

	bool hdpi_enabled;
	bool hardware_mouse_enabled;
	bool fb_enabled; /* enable legacy framebuffer */

	LIST_HEAD(scanouts, vgpu_scanout) scanouts;
};

extern uuid_t vm_uuid;

static void pci_vgpu_reset(void *vsc);
static int pci_vgpu_cfgread(void *vsc, int offset, int size, uint32_t *retval);
static int pci_vgpu_cfgwrite(void *vsc, int offset, int size, uint32_t value);
static void pci_vgpu_neg_features(void *vsc, uint64_t negotiated_features);
static void pci_vgpu_destroy_scanouts(struct pci_vgpu_softc *sc);
static bool pci_vgpu_host_display_info(uint32_t *logical_width,
    uint32_t *logical_height, uint32_t *pixel_width, uint32_t *pixel_height);
static bool pci_vgpu_host_physical_size(uint32_t *width_mm,
    uint32_t *height_mm);
static void pci_vgpu_edid_physical_size(struct pci_vgpu_softc *sc,
    uint32_t *width_mm, uint32_t *height_mm);

static struct virtio_consts vgpu_vi_consts = {
	.vc_name = "vgpu",
	.vc_nvq = VGPU_MAXQ - 1,
	.vc_cfgsize = sizeof(struct virtio_gpu_config),
	.vc_reset = pci_vgpu_reset,
	.vc_cfgread = pci_vgpu_cfgread,
	.vc_cfgwrite = pci_vgpu_cfgwrite,
	.vc_apply_features = pci_vgpu_neg_features,
	.vc_hv_caps = VGPU_S_HOSTCAPS | VIRTIO_F_VERSION_1 |
	    (1ULL << VIRTIO_GPU_F_EDID),
#ifdef BHYVE_SNAPSHOT
	.vc_pause = pci_vgpu_pause,
	.vc_resume = pci_vgpu_resume,
	.vc_snapshot = pci_vgpu_snapshot,
#endif
};

static void
pci_vgpu_neg_features(void *vsc, uint64_t negotiated_features)
{
	struct pci_vgpu_softc *sc = vsc;

	sc->vsc_features = negotiated_features;
	DPRINTF(("vgpu: pci_vgpu_neg_features 0x%llx", negotiated_features));
}

static int
pci_vgpu_cfgwrite(void *vsc, int offset, int size, uint32_t value)
{
	struct pci_vgpu_softc *sc = vsc;

	switch (offset) {
	case 4:
		// events clear
		DPRINTF(("vgpu: clear events"));
		sc->vsc_config.events_read &= ~value;
		break;
	default:
		/* silently ignore writes */
		DPRINTF(("vgpu: write to readonly reg 0x%x", offset));
		break;
	}

	return (0);
}

static int
pci_vgpu_cfgread(void *vsc, int offset, int size, uint32_t *retval)
{
	struct pci_vgpu_softc *sc = vsc;
	void *ptr;

	DPRINTF(("vgpu: pci_vgpu_cfgread 0x%x", offset));

	ptr = (uint8_t *)&sc->vsc_config + offset;
	memcpy(retval, ptr, size);
	return (0);
}

static bool
pci_vgpu_mode_supports_hdpi(uint32_t width, uint32_t height)
{
	return (width <= COLS_MAX / 2 && height <= ROWS_MAX / 2);
}

static uint32_t
pci_vgpu_host_scale(void)
{
	uint32_t logical_width, logical_height, pixel_width, pixel_height;
	uint32_t scale;

	if (!pci_vgpu_host_display_info(&logical_width, &logical_height,
		&pixel_width, &pixel_height)) {
		return (1);
	}

	scale = MIN(pixel_width / logical_width, pixel_height / logical_height);
	return (MAX(scale, 1));
}

static bool
pci_vgpu_host_display_info(uint32_t *logical_width, uint32_t *logical_height,
    uint32_t *pixel_width, uint32_t *pixel_height)
{
#ifdef __APPLE__
	CGDirectDisplayID display;
	CGDisplayModeRef mode;

	display = CGMainDisplayID();
	mode = CGDisplayCopyDisplayMode(display);
	if (mode == NULL)
		return (false);

	*logical_width = (uint32_t)CGDisplayModeGetWidth(mode);
	*logical_height = (uint32_t)CGDisplayModeGetHeight(mode);
	*pixel_width = (uint32_t)CGDisplayModeGetPixelWidth(mode);
	*pixel_height = (uint32_t)CGDisplayModeGetPixelHeight(mode);
	CGDisplayModeRelease(mode);

	return (*logical_width > 0 && *logical_height > 0 &&
	    *pixel_width > 0 && *pixel_height > 0);
#else
	(void)logical_width;
	(void)logical_height;
	(void)pixel_width;
	(void)pixel_height;
	return (false);
#endif
}

static bool
pci_vgpu_host_physical_size(uint32_t *width_mm, uint32_t *height_mm)
{
#ifdef __APPLE__
	CGSize size;

	size = CGDisplayScreenSize(CGMainDisplayID());
	if (size.width <= 0 || size.height <= 0)
		return (false);

	*width_mm = (uint32_t)size.width;
	*height_mm = (uint32_t)size.height;
	return (*width_mm >= 100 && *height_mm >= 80);
#else
	(void)width_mm;
	(void)height_mm;
	return (false);
#endif
}

static uint32_t
pci_vgpu_physical_size_for_dpi(uint32_t pixels)
{
	return (MAX(pixels * VGPU_EDID_MM_PER_INCH_X10 /
	    (VGPU_EDID_DPI * 10), 1));
}

static void
pci_vgpu_edid_physical_size(struct pci_vgpu_softc *sc, uint32_t *width_mm,
    uint32_t *height_mm)
{
	uint32_t host_width_mm, host_height_mm;
	uint32_t virtual_width_mm, virtual_height_mm;
	bool has_host_size;

	host_width_mm = 0;
	host_height_mm = 0;
	has_host_size = pci_vgpu_host_physical_size(&host_width_mm,
	    &host_height_mm);

	virtual_width_mm = pci_vgpu_physical_size_for_dpi(sc->start_resx);
	virtual_height_mm = pci_vgpu_physical_size_for_dpi(sc->start_resy);

	if (sc->hdpi_enabled) {
		*width_mm = has_host_size ? MIN(host_width_mm, virtual_width_mm) :
		    virtual_width_mm;
		*height_mm = has_host_size ? MIN(host_height_mm, virtual_height_mm) :
		    virtual_height_mm;
		return;
	}

	if (has_host_size) {
		*width_mm = host_width_mm;
		*height_mm = host_height_mm;
		return;
	}

	*width_mm = virtual_width_mm;
	*height_mm = virtual_height_mm;
}

static void
pci_vgpu_set_effective_resolution(struct pci_vgpu_softc *sc)
{
	uint32_t factor;

	sc->resx = sc->start_resx;
	sc->resy = sc->start_resy;
	if (sc->hdpi_enabled) {
		factor = MAX(sc->host_scale, 1);
		sc->resx *= factor;
		sc->resy *= factor;
	}
}

static void
pci_vgpu_reset(void *vsc)
{
	struct pci_vgpu_softc *sc = vsc;

	DPRINTF(("vgpu: device reset requested !"));
	console_set_scanout(false, 0, 0, 0, 0, NULL, 0, false);
	pci_vgpu_destroy_scanouts(sc);

	pci_vgpu_set_effective_resolution(sc);
	vi_reset_dev(&sc->vsc_vs);
}

static void
pci_vgpu_prepare_response(struct virtio_gpu_ctrl_hdr *req,
    struct virtio_gpu_ctrl_hdr *rsp, uint32_t rsp_type)
{
	bzero(rsp, sizeof(*rsp));
	rsp->ctx_id = req->ctx_id;
	rsp->fence_id = req->fence_id;
	rsp->ring_idx = req->ring_idx;
	rsp->type = htole32(rsp_type);
	if (le32toh(req->flags) & VIRTIO_GPU_FLAG_FENCE) {
		rsp->flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	}
}

static size_t
pci_vgpu_get_display_info(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_resp_display_info di;
	size_t len;

	bzero(&di, sizeof(di));
	pci_vgpu_prepare_response(req, &di.hdr,
	    VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
	di.pmodes[0].enabled = htole32(1);
	di.pmodes[0].r.width = htole32(sc->resx);
	di.pmodes[0].r.height = htole32(sc->resy);

	DPRINTF(("get_display_info %d %d", sc->resx, sc->resy));

	if (rsp_len < sizeof(di))
		return (0);
	len = sizeof(di);
	memcpy(rsp, &di, len);
	return (len);
}

static struct vgpu_scanout *
pci_vgpu_find_resource(struct pci_vgpu_softc *sc, uint32_t resource_id)
{
	struct vgpu_scanout *scanout;
	LIST_FOREACH(scanout, &sc->scanouts, entries) {
		if (scanout->resource_id == resource_id) {
			return (scanout);
		}
	}
	return (NULL);
}

static void
pci_vgpu_destroy_scanout(struct vgpu_scanout *scanout)
{
	LIST_REMOVE(scanout, entries);
	munmap(scanout->base_ptr, scanout->size);
	shm_unlink(scanout->shm_name);
	close(scanout->shm_fd);
	free(scanout->backing_iov);
	free(scanout);
}

static void
pci_vgpu_destroy_scanouts(struct pci_vgpu_softc *sc)
{
	struct vgpu_scanout *scanout, *tmp;

	LIST_FOREACH_SAFE(scanout, &sc->scanouts, entries, tmp) {
		if (sc->fb_enabled &&
		    scanout->resource_id == VGPU_LEGACY_RESOURCE_ID) {
			continue;
		}
		pci_vgpu_destroy_scanout(scanout);
	}
}

static int
pci_vgpu_create_scanout(struct pci_vgpu_softc *sc, uint32_t resource_id,
    uint32_t width, u_int32_t height, uint32_t sc_size, uint32_t format)
{
	struct vgpu_scanout *scanout;
	uuid_string_t uuid;

	scanout = calloc(1, sizeof(struct vgpu_scanout));
	if (scanout == NULL)
		return (-1);

	scanout->resource_id = resource_id;
	scanout->width = width;
	scanout->height = height;
	scanout->format = format;
	scanout->stride = roundup2(width * 4, 32);

	uuid_unparse(vm_uuid, uuid);
	uuid[20] = 0;
	snprintf(scanout->shm_name, sizeof(scanout->shm_name), "/%s-%08x", uuid,
	    scanout->resource_id);

	/*
	 * Darwin returns EINVAL if ftruncate() is repeated on an existing
	 * POSIX shm object. A crash can leave the object behind, so always
	 * start scanouts with a fresh name instance.
	 */
	if (shm_unlink(scanout->shm_name) == -1 && errno != ENOENT) {
		EPRINTLN("shm_unlink stale %s: %s", scanout->shm_name,
		    strerror(errno));
		free(scanout);
		return (-1);
	}

	scanout->shm_fd = shm_open(scanout->shm_name,
	    O_CREAT | O_EXCL | O_RDWR,
	    S_IRUSR | S_IWUSR);
	if (scanout->shm_fd == -1) {
		EPRINTLN("shm_open %s: %s", scanout->shm_name,
		    strerror(errno));
		free(scanout);
		return (-1);
	}

	// Resize the shared memory
	if (ftruncate(scanout->shm_fd, sc_size) == -1) {
		EPRINTLN("ftruncate %s, size %u: %s", scanout->shm_name,
		    sc_size, strerror(errno));
		close(scanout->shm_fd);
		shm_unlink(scanout->shm_name);
		free(scanout);
		return (-1);
	}

	scanout->base_ptr = mmap(NULL, sc_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, scanout->shm_fd, 0);
	if (scanout->base_ptr == MAP_FAILED) {
		EPRINTLN("mmap %s, size %u: %s", scanout->shm_name,
		    sc_size, strerror(errno));
		close(scanout->shm_fd);
		shm_unlink(scanout->shm_name);
		free(scanout);
		return (-1);
	}
	scanout->size = sc_size;
	DPRINTF(("pci_vgpu_create_scanout: shared mem created %s, %u",
	    scanout->shm_name, sc_size));

	LIST_INSERT_HEAD(&sc->scanouts, scanout, entries);
	return (0);
}

static size_t
pci_vgpu_create_resource_2d(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_resource_create_2d *res;
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len, sc_size;
	struct vgpu_scanout *scanout;

	res = (struct virtio_gpu_resource_create_2d *)req;

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);
	memcpy(rsp, &hdr, len);

	DPRINTF(("create_resource_2d %d, w %d h %d", le32toh(res->resource_id),
	    le32toh(res->width), le32toh(res->height)));
	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (scanout) {
		// already exists
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return len;
	}

	sc_size = roundup2(le32toh(res->width) * 4, 32) * le32toh(res->height);
	if (pci_vgpu_create_scanout(sc, le32toh(res->resource_id),
		le32toh(res->width), le32toh(res->height), sc_size,
		le32toh(res->format)) != 0) {
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
		memcpy(rsp, &hdr, len);
	}
	return (len);
}

static size_t
pci_vgpu_resource_unref(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_resource_unref *res;
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len;
	struct vgpu_scanout *scanout;

	res = (struct virtio_gpu_resource_unref *)req;

	DPRINTF(("resource_unref %d", le32toh(res->resource_id)));
	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);
	memcpy(rsp, &hdr, len);

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	if (sc->fb_enabled && scanout->resource_id == VGPU_LEGACY_RESOURCE_ID) {
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	pci_vgpu_destroy_scanout(scanout);

	return (len);
}

static void
pci_vgpu_render(void *arg)
{
}

static size_t
pci_vgpu_resource_attach_backing(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, size_t req_len, uint8_t *rsp,
    size_t rsp_len)
{
	struct virtio_gpu_resource_attach_backing *res;
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_mem_entry *mem;
	size_t len, min_req_len;
	uint32_t i, nr_entries;
	struct vgpu_scanout *scanout;

	res = (struct virtio_gpu_resource_attach_backing *)req;
	nr_entries = le32toh(res->nr_entries);
	DPRINTF(("resource_attach_backing: %d, res %d", nr_entries,
	    le32toh(res->resource_id)));

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);
	memcpy(rsp, &hdr, len);

	if (!nr_entries)
		return len;

	if ((size_t)nr_entries > (SIZE_MAX - sizeof(*res)) / sizeof(*mem)) {
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return len;
	}
	min_req_len = sizeof(*res) + nr_entries * sizeof(*mem);
	if (req_len < min_req_len) {
		WPRINTF(("vgpu: short attach backing resource=%d len=%zu "
		    "required=%zu entries=%u", le32toh(res->resource_id),
		    req_len, min_req_len, nr_entries));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return len;
	}

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return len;
	}

	free(scanout->backing_iov);
	scanout->backing_iov = calloc(nr_entries, sizeof(struct iovec));
	if (scanout->backing_iov == NULL) {
		scanout->iov_cnt = 0;
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
		memcpy(rsp, &hdr, len);
		return len;
	}
	scanout->iov_cnt = nr_entries;

	// map guest pages into host contiguos memorry
	mem = (struct virtio_gpu_mem_entry *)(res + 1);
	for (i = 0; i < nr_entries; i++) {
		scanout->backing_iov[i].iov_base = vm_map_gpa(
		    sc->vsc_vs.vs_pi->pi_vmctx, le64toh(mem[i].addr),
		    le32toh(mem[i].length));
		scanout->backing_iov[i].iov_len = le32toh(mem[i].length);

		if (scanout->backing_iov[i].iov_base == NULL) {
			WPRINTF(("vgpu: failed to map backing resource=%d "
			    "entry=%u addr=%llx len=%u",
			    le32toh(res->resource_id), i,
			    (unsigned long long)le64toh(mem[i].addr),
			    le32toh(mem[i].length)));
			free(scanout->backing_iov);
			scanout->backing_iov = NULL;
			scanout->iov_cnt = 0;
			hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
			memcpy(rsp, &hdr, len);
			return len;
		}
	}
	bzero((void *)scanout->base_ptr, scanout->size);

	return (len);
}

static size_t
pci_vgpu_set_scanout(struct pci_vgpu_softc *sc, struct virtio_gpu_ctrl_hdr *req,
    uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_set_scanout *res;
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len;
	struct vgpu_scanout *scanout;

	res = (struct virtio_gpu_set_scanout *)req;

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);
	memcpy(rsp, &hdr, len);

	if (le32toh(res->resource_id)) {
		uint32_t width, height;

		width = le32toh(res->r.width);
		height = le32toh(res->r.height);
		scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
		if (!scanout) {
			EPRINTLN("pci_vgpu: scanout %d not found",
			    le32toh(res->resource_id));
			hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
			memcpy(rsp, &hdr, len);
			return (len);
		}
		if (width > scanout->width || height > scanout->height) {
			WPRINTF(("vgpu: invalid set_scanout resource=%d "
			    "rect=%ux%u resource=%ux%u",
			    le32toh(res->resource_id), width, height,
			    scanout->width, scanout->height));
			hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
			memcpy(rsp, &hdr, len);
			return (len);
		}

		console_set_scanout(true, width, height, scanout->stride,
		    scanout->format, scanout->shm_name, scanout->size, false);
		DPRINTF(("active scanout: %d: %dx%d, %d",
		    scanout->resource_id, width, height, scanout->iov_cnt));
	} else {
		console_set_scanout(false, 0, 0, 0, 0, NULL, 0, false);
		DPRINTF(("active scanout disabled"));
	}

	return (len);
}

static size_t
pci_vgpu_resource_dettach_backing(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_resource_detach_backing *res;
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len;
	struct vgpu_scanout *scanout;

	res = (struct virtio_gpu_resource_detach_backing *)req;

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);
	memcpy(rsp, &hdr, len);

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	scanout->iov_cnt = 0;
	free(scanout->backing_iov);
	scanout->backing_iov = NULL;

	return (len);
}

static size_t
pci_vgpu_flush(struct pci_vgpu_softc *sc, struct virtio_gpu_ctrl_hdr *req,
    uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_resource_flush *res;
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len;
	struct vgpu_scanout *scanout;
	char notification[1024];

	res = (struct virtio_gpu_resource_flush *)req;

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);
	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}
	msync(scanout->base_ptr, scanout->size, MS_SYNC);

	memcpy(rsp, &hdr, len);
	snprintf(notification, sizeof(notification),
	    "{ \"event\": \"update_scanout\", \
            \"data\": {\
                \"x\": %d, \
                \"y\": %d, \
                \"width\": %d, \
                \"height\": %d, \
            } \
        }",
	    le32toh(res->r.x), le32toh(res->r.y), le32toh(res->r.width),
	    le32toh(res->r.height));
	cnc_send_notification(notification);

	return (len);
}

static size_t
pci_vgpu_transfer_to_host_2d(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_transfer_to_host_2d *res;
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len;
	struct vgpu_scanout *scanout;
	int h;
	uint32_t x, y, width, height, req_width, req_height;
	size_t src_stride_width;
	size_t backing_size, req_offset, src_end, src_off, src_stride;
	size_t dst_end, dst_off, dst_stride, len_to_copy;

	res = (struct virtio_gpu_transfer_to_host_2d *)req;
	x = le32toh(res->r.x);
	y = le32toh(res->r.y);
	width = le32toh(res->r.width);
	height = le32toh(res->r.height);
	req_width = width;
	req_height = height;
	if (x != 0 || y != 0 || width != sc->resx || height != sc->resy) {
		DPRINTF(("transfer_to_host_2d %d %d %d %d", x, y, width,
		    height));
	}

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);
	memcpy(rsp, &hdr, len);

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	if (scanout->backing_iov == NULL || scanout->iov_cnt <= 0 ||
	    scanout->base_ptr == NULL) {
		WPRINTF(("vgpu: transfer without backing resource=%d",
		    le32toh(res->resource_id)));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	if (width == 0 || height == 0)
		return (len);

	if (x >= scanout->width || y >= scanout->height)
		return (len);

	if (width > scanout->width - x || height > scanout->height - y) {
		width = MIN(width, scanout->width - x);
		height = MIN(height, scanout->height - y);
		WPRINTF(("vgpu: clipped transfer outside scanout resource=%d "
		    "rect=%ux%u+%u+%u clipped=%ux%u scanout=%ux%u",
		    le32toh(res->resource_id), req_width, req_height, x, y,
		    width, height, scanout->width, scanout->height));
	}

	if (width == 0 || height == 0)
		return (len);

	if ((size_t)x + req_width > SIZE_MAX / 4 ||
	    (size_t)scanout->width > SIZE_MAX / 4) {
		WPRINTF(("vgpu: transfer stride overflow resource=%d "
		    "rect=%ux%u+%u+%u scanout=%ux%u",
		    le32toh(res->resource_id), req_width, req_height, x, y,
		    scanout->width, scanout->height));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	dst_stride = scanout->stride;
	src_stride_width = MAX((size_t)scanout->width, (size_t)x + req_width);
	src_stride = src_stride_width * 4;
	len_to_copy = width * 4;
	dst_end = dst_stride * (y + height - 1) + x * 4 + len_to_copy;
	if (dst_end > scanout->size) {
		WPRINTF(("vgpu: transfer destination overflow resource=%d "
		    "end=%zu size=%zu", le32toh(res->resource_id), dst_end,
		    scanout->size));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	backing_size = count_iov(scanout->backing_iov, scanout->iov_cnt);
	req_offset = le64toh(res->offset);
	if (req_offset > backing_size) {
		WPRINTF(("vgpu: transfer source offset overflow resource=%d "
		    "offset=%zu backing=%zu", le32toh(res->resource_id),
		    req_offset, backing_size));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}
	src_end = req_offset + src_stride * (height - 1) + len_to_copy;
	if (src_end > backing_size && src_stride_width != scanout->width) {
		src_stride_width = scanout->width;
		src_stride = src_stride_width * 4;
		src_end = req_offset + src_stride * (height - 1) + len_to_copy;
	}
	if (src_end > backing_size) {
		WPRINTF(("vgpu: transfer source overflow resource=%d "
		    "end=%zu backing=%zu", le32toh(res->resource_id), src_end,
		    backing_size));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		memcpy(rsp, &hdr, len);
		return (len);
	}

	if (x != 0 || len_to_copy != src_stride || dst_stride != src_stride) {
		for (h = 0; h < height; h++) {
			src_off = req_offset + src_stride * h;
			dst_off = dst_stride * (y + h) + x * 4;
			iov_copy(scanout->base_ptr + dst_off, len_to_copy,
			    scanout->backing_iov,
			    scanout->iov_cnt, src_off);
		}
	} else {
		src_off = req_offset;
		dst_off = y * dst_stride;
		iov_copy(scanout->base_ptr + dst_off, height * dst_stride,
		    scanout->backing_iov,
		    scanout->iov_cnt, src_off);
	}
	return (len);
}

static size_t pci_vgpu_default_response(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len,
    uint32_t code);

static void
pci_vgpu_interrupt_config(struct pci_vgpu_softc *sc)
{
	uint16_t msix_idx;

	msix_idx = sc->vsc_vs.vs_msix_cfg_idx;
	if (msix_idx == VIRTIO_MSI_NO_VECTOR)
		msix_idx = sc->vsc_queues[VGPU_CTRL].vq_msix_idx;

	vi_interrupt(&sc->vsc_vs, VIRTIO_PCI_ISR_CONFIG, msix_idx);
}

static size_t
pci_vgpu_get_edid(struct pci_vgpu_softc *sc, struct virtio_gpu_ctrl_hdr *req,
    uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_cmd_get_edid *cmd;
	struct virtio_gpu_resp_edid edid;
	size_t len, size;
	uint32_t logical_width, logical_height;
	uint32_t physical_width_mm, physical_height_mm;

	cmd = (struct virtio_gpu_cmd_get_edid *)req;
	if (le32toh(cmd->scanout) >= sc->vsc_config.num_scanouts) {
		return (pci_vgpu_default_response(sc, req, rsp, rsp_len,
		    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID));
	}

	bzero(&edid, sizeof(edid));
	pci_vgpu_prepare_response(req, &edid.hdr, VIRTIO_GPU_RESP_OK_EDID);
	logical_width = sc->start_resx;
	logical_height = sc->start_resy;
	pci_vgpu_edid_physical_size(sc, &physical_width_mm,
	    &physical_height_mm);
	if (sc->hdpi_enabled) {
		logical_width *= MAX(sc->host_scale, 1);
		logical_height *= MAX(sc->host_scale, 1);
	}
	DPRINTF(("vgpu: get_edid preferred=%ux%u logical=%ux%u "
	    "physical=%ummx%umm hdpi=%d", sc->resx, sc->resy,
	    logical_width, logical_height, physical_width_mm,
	    physical_height_mm, sc->hdpi_enabled));
	size = generate_edid(sc->resx, sc->resy, logical_width, logical_height,
	    physical_width_mm, physical_height_mm, sc->hdpi_enabled,
	    sc->max_resx, sc->max_resy, edid.edid);
	edid.size = htole32(size);

	len = MIN(rsp_len, sizeof(edid));
	memcpy(rsp, &edid, len);
	return (len);
}

static size_t
pci_vgpu_default_response(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len,
    uint32_t code)
{
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len;

	pci_vgpu_prepare_response(req, &hdr, code);

	if (rsp_len < sizeof(hdr))
		return (0);
	len = sizeof(hdr);
	memcpy(rsp, &hdr, len);

	return (len);
}

static size_t
pci_vgpu_process_cmd(struct pci_vgpu_softc *sc, struct virtio_gpu_ctrl_hdr *req,
    size_t req_len, uint8_t *rsp, size_t rsp_len)
{
	size_t len = 0;

	DPRINTF(("pci_vgpu_process_cmd %d, fence %lld", le32toh(req->type),
	    req->fence_id));

	switch (le32toh(req->type)) {
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
		len = pci_vgpu_get_display_info(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
		len = pci_vgpu_create_resource_2d(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_RESOURCE_UNREF:
		len = pci_vgpu_resource_unref(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_SET_SCANOUT:
		len = pci_vgpu_set_scanout(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
		len = pci_vgpu_flush(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
		len = pci_vgpu_transfer_to_host_2d(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
		len = pci_vgpu_resource_attach_backing(sc, req, req_len, rsp,
		    rsp_len);
		break;
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
		len = pci_vgpu_resource_dettach_backing(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_GET_EDID:
		len = pci_vgpu_get_edid(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
	case VIRTIO_GPU_CMD_GET_CAPSET:
	case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID:
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
	case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
	default:
		len = pci_vgpu_default_response(sc, req, rsp, rsp_len,
		    VIRTIO_GPU_RESP_ERR_UNSPEC);
		break;
	}

	return (len);
}

static void
pci_vgpu_ping_ctrl(void *vsc, struct vqueue_info *vq)
{
	struct pci_vgpu_softc *sc = vsc;
	struct vi_req req;
	struct iovec iov[VQ_MAX_DESC];
	size_t rsp_len;
	size_t rsp_buf_len;
	ssize_t cmd_len;
	struct virtio_gpu_ctrl_hdr *cmd;
	uint8_t *rsp_buf;

	// DPRINTF(("vgpu: pci_vgpu_ping_ctrl"));
	pthread_mutex_lock(&sc->vsc_mtx);
	// vq_kick_disable(vq);

	while (vq_has_descs(vq)) {
		int n = vq_getchain(vq, iov, VQ_MAX_DESC, &req);

		rsp_len = 0;
		rsp_buf_len = 0;
		cmd = NULL;
		rsp_buf = NULL;
		if (n <= 0 || n > VQ_MAX_DESC ||
		    n != req.readable + req.writable || req.readable == 0) {
			WPRINTF(("vgpu: invalid ctrl chain n=%d readable=%d "
			    "writable=%d", n, req.readable, req.writable));
			vq_relchain(vq, req.idx, 0);
			continue;
		}

		if (req.writable == 0) {
			WPRINTF(("vgpu: ctrl command without response descriptor, "
			    "readable=%d", req.readable));
			vq_relchain(vq, req.idx, 0);
			continue;
		}

		rsp_buf_len = count_iov(&iov[req.readable], req.writable);
		if (rsp_buf_len == 0) {
			WPRINTF(("vgpu: ctrl command with empty response "
			    "descriptors, readable=%d writable=%d",
			    req.readable, req.writable));
			vq_relchain(vq, req.idx, 0);
			continue;
		}
		rsp_buf = calloc(1, rsp_buf_len);
		if (rsp_buf == NULL) {
			WPRINTF(("vgpu: failed to allocate response buffer "
			    "len=%zu", rsp_buf_len));
			vq_relchain(vq, req.idx, 0);
			continue;
		}

		cmd_len = iov_to_buf(iov, req.readable, (void **)&cmd);
		if (cmd_len > 0) {
			rsp_len = pci_vgpu_process_cmd(sc, cmd, (size_t)cmd_len,
			    rsp_buf, rsp_buf_len);
			if (rsp_len > rsp_buf_len) {
				WPRINTF(("vgpu: response too large len=%zu "
				    "available=%zu", rsp_len, rsp_buf_len));
				rsp_len = rsp_buf_len;
			}
			buf_to_iov(rsp_buf, rsp_len, &iov[req.readable],
			    req.writable, 0);
			free(cmd);
		}
		free(rsp_buf);
		vq_relchain(vq, req.idx, rsp_len);
	}
	// vq_kick_enable(vq);
	vq_endchains(vq, 1);

	pthread_mutex_unlock(&sc->vsc_mtx);
}

static size_t
pci_vgpu_update_cursor(struct pci_vgpu_softc *sc,
    struct virtio_gpu_update_cursor *req)
{
	struct vgpu_scanout *scanout;

	if (!sc->hardware_mouse_enabled)
		return (-1);

	if (!req->resource_id) {
		console_set_mouse_scanout(false, 0, 0, 0, 0, 0, 0, NULL);
		return (0);
	}

	scanout = pci_vgpu_find_resource(sc, le32toh(req->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(req->resource_id));
		return (0);
	}

	console_set_mouse_scanout(true, le32toh(scanout->width),
	    le32toh(scanout->height), scanout->stride,
	    le32toh(scanout->format),
	    le32toh(req->hot_x), le32toh(req->hot_y), scanout->shm_name);
	return (0);
}

static size_t
pci_vgpu_move_cursor(struct pci_vgpu_softc *sc,
    struct virtio_gpu_update_cursor *req)
{
	char notification[1024];

	snprintf(notification, sizeof(notification),
	    "{ \"event\": \"move_cursor\","
	    "\"data\": {"
	    "\"x\": %d,"
	    "\"y\": %d,"
	    "}"
	    "}",
	    le32toh(req->pos.x), le32toh(req->pos.y));
	cnc_send_notification(notification);
	return (0);
}

static size_t
pci_vgpu_process_cursor(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, void *rsp, size_t rsp_len)
{
	struct virtio_gpu_update_cursor *cursor;
	struct virtio_gpu_ctrl_hdr hdr;
	size_t len;

	cursor = (struct virtio_gpu_update_cursor *)req;
	DPRINTF(("res %d, scanout %d, type %d", cursor->resource_id,
	    cursor->pos.scanout_id, cursor->hdr.type));

	len = 0;
	if (rsp_len) {
		pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);
		len = MIN(rsp_len, sizeof(hdr));
	}

	switch (le32toh(req->type)) {
	case VIRTIO_GPU_CMD_UPDATE_CURSOR:
		if (pci_vgpu_update_cursor(sc, cursor) < 0)
			hdr.type = htole32(
			    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		break;
	case VIRTIO_GPU_CMD_MOVE_CURSOR:
		pci_vgpu_move_cursor(sc, cursor);
		break;
	default:
		EPRINTLN("pci_vgpu_update_cursor: unknown cursor command %d",
		    le32toh(req->type));
		break;
	}

	if (rsp_len)
		memcpy(rsp, &hdr, len);
	return (len);
}

static void
pci_vgpu_ping_cursor(void *vsc, struct vqueue_info *vq)
{
	struct pci_vgpu_softc *sc = vsc;
	struct vi_req req;
	struct iovec iov[VQ_MAX_DESC];
	size_t rsp_len;
	struct virtio_gpu_ctrl_hdr *cmd;

	pthread_mutex_lock(&sc->vsc_mtx);
	// vq_kick_disable(vq);

	DPRINTF(("vgpu: pci_vgpu_ping_cursor"));

	while (vq_has_descs(vq)) {
		vq_getchain(vq, iov, VQ_MAX_DESC, &req);

		cmd = NULL;
		if (iov_to_buf(iov, req.readable, (void **)&cmd) > 0) {
			rsp_len = pci_vgpu_process_cursor(sc, cmd,
			    req.writable ? iov[req.readable].iov_base : NULL,
			    req.writable ? iov[req.readable].iov_len : 0);
			free(cmd);
		}
		vq_relchain(vq, req.idx, rsp_len);
	}
	// vq_kick_enable(vq);
	vq_endchains(vq, 1);

	pthread_mutex_unlock(&sc->vsc_mtx);
}

static void
resize_event(int x, int y, void *arg)
{
	struct pci_vgpu_softc *sc;
	bool notify;

	sc = (struct pci_vgpu_softc *)arg;
	pthread_mutex_lock(&sc->vsc_mtx);

	DPRINTF(("resize_event %d %d", x, y));
	if (x >= COLS_MIN && x <= COLS_MAX && y >= ROWS_MIN &&
	    y <= ROWS_MAX) {
		if (sc->resx == (uint32_t)x && sc->resy == (uint32_t)y) {
			DPRINTF(("resize_event ignored duplicate %d %d", x, y));
			pthread_mutex_unlock(&sc->vsc_mtx);
			return;
		}
		sc->resx = x;
		sc->resy = y;
		notify = (sc->vsc_config.events_read &
		    VIRTIO_GPU_EVENT_DISPLAY) == 0;
		sc->vsc_config.events_read |= VIRTIO_GPU_EVENT_DISPLAY;
		if (notify)
			pci_vgpu_interrupt_config(sc);
	}
	pthread_mutex_unlock(&sc->vsc_mtx);
}

static int
pci_vgpu_init(struct pci_devinst *pi, nvlist_t *nvl)
{
	struct pci_vgpu_softc *sc;
	const char *value;
	int err;
	pthread_mutexattr_t attr;
	uint32_t host_logical_width, host_logical_height;
	uint32_t host_pixel_width, host_pixel_height;

	/*
	 * Allocate data structures for further virtio initializations.
	 * sc also contains a copy of vtnet_vi_consts, since capabilities
	 * change depending on the backend.
	 */
	sc = calloc(1, sizeof(struct pci_vgpu_softc));

	sc->vsc_consts = vgpu_vi_consts;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sc->vsc_mtx, &attr);

	sc->vsc_queues[VGPU_CTRL].vq_qsize = VGPU_RINGSZ;
	sc->vsc_queues[VGPU_CTRL].vq_notify = pci_vgpu_ping_ctrl;
	sc->vsc_queues[VGPU_CURSOR].vq_qsize = VGPU_RINGSZ;
	sc->vsc_queues[VGPU_CURSOR].vq_notify = pci_vgpu_ping_cursor;

	sc->hardware_mouse_enabled = true;
	value = get_config_value_node(nvl, "hardware_mouse");
	if (value != NULL) {
		if (!strcmp(value, "on") || !strcmp(value, "true") ||
		    !strcmp(value, "yes")) {
			sc->hardware_mouse_enabled = true;
		} else if (!strcmp(value, "off") || !strcmp(value, "false") ||
		    !strcmp(value, "no")) {
			sc->hardware_mouse_enabled = false;
		} else {
			EPRINTLN("vgpu: invalid hardware_mouse value '%s'", value);
			return (-1);
		}
	}

	value = get_config_value_node(nvl, "fb");
	if (value && !strcmp(value, "on"))
		sc->fb_enabled = true;

	value = get_config_value_node(nvl, "hdpi");
	if (value != NULL) {
		if (!strcmp(value, "on") || !strcmp(value, "true") ||
		    !strcmp(value, "yes")) {
			sc->hdpi_enabled = true;
		} else if (!strcmp(value, "off") || !strcmp(value, "false") ||
		    !strcmp(value, "no")) {
			sc->hdpi_enabled = false;
		} else {
			EPRINTLN("vgpu: invalid hdpi value '%s'", value);
			return (-1);
		}
	}

	/* initialize config space */
	pci_set_cfgdata16(pi, PCIR_DEVICE, VIRTIO_DEV_GPU);
	pci_set_cfgdata16(pi, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_DISPLAY);
	pci_set_cfgdata8(pi, PCIR_SUBCLASS, PCIS_DISPLAY_OTHER);
	if (sc->fb_enabled)
		pci_set_cfgdata16(pi, PCIR_SUBDEV_0, 0x4690);
	else
		pci_set_cfgdata16(pi, PCIR_SUBDEV_0, 0x4680);
	pci_set_cfgdata16(pi, PCIR_SUBVEND_0, 0xFB5D);
	pci_set_cfgdata16(pi, PCIR_REVID, 1);

	sc->vsc_config.num_scanouts = 1;
	sc->vsc_config.num_capsets = 0;

	vi_softc_linkup(&sc->vsc_vs, &sc->vsc_consts, sc, pi, sc->vsc_queues);
	sc->vsc_vs.vs_mtx = &sc->vsc_mtx;

	/* use BAR 3 to map MSI-X table and PBA, if we're using MSI-X */
	if (vi_intr_init(&sc->vsc_vs, 3, msix_supported())) {
		free(sc);
		return (1);
	}

	/* use BAR 2 to map config regs in MMIO space */
	vi_set_mmio_bar_modern(&sc->vsc_vs, 2);

	err = add_virtio10_pci_caps(&sc->vsc_vs, 2);
	if (err) {
		return (err);
	}

	value = get_config_value_node(nvl, "w");
	if (value)
		sc->start_resx = strtol(value, NULL, 10);
	else
		sc->start_resx = COLS_DEFAULT;

	value = get_config_value_node(nvl, "h");
	if (value)
		sc->start_resy = strtol(value, NULL, 10);
	else
		sc->start_resy = ROWS_DEFAULT;

	if (sc->start_resx > COLS_MAX || sc->start_resy > ROWS_MAX) {
		EPRINTLN("fbuf: max resolution is %ux%u", COLS_MAX, ROWS_MAX);
		return (-1);
	}
	if (sc->start_resx < COLS_MIN || sc->start_resy < ROWS_MIN) {
		EPRINTLN("vgpu: minimum resolution is %ux%u", COLS_MIN,
		    ROWS_MIN);
		return (-1);
	}

	sc->max_resx = COLS_MAX;
	sc->max_resy = ROWS_MAX;
	sc->host_scale = pci_vgpu_host_scale();
	if (pci_vgpu_host_display_info(&host_logical_width,
		&host_logical_height, &host_pixel_width,
		&host_pixel_height)) {
		if (sc->hdpi_enabled) {
			sc->max_resx = MIN(host_pixel_width, COLS_MAX);
			sc->max_resy = MIN(host_pixel_height, ROWS_MAX);
		} else {
			sc->max_resx = MIN(host_logical_width, COLS_MAX);
			sc->max_resy = MIN(host_logical_height, ROWS_MAX);
		}
	}
	if (sc->hdpi_enabled &&
	    !pci_vgpu_mode_supports_hdpi(sc->start_resx, sc->start_resy)) {
		EPRINTLN("vgpu: hdpi resolution %ux%u exceeds max %ux%u",
		    sc->start_resx * 2, sc->start_resy * 2, COLS_MAX,
		    ROWS_MAX);
		return (-1);
	}
	pci_vgpu_set_effective_resolution(sc);
	DPRINTF(("vgpu: hdpi=%d host_scale=%u logical=%ux%u "
	    "effective=%ux%u max=%ux%u", sc->hdpi_enabled, sc->host_scale,
	    sc->start_resx, sc->start_resy, sc->resx, sc->resy,
	    sc->max_resx, sc->max_resy));

	LIST_INIT(&sc->scanouts);

	console_fb_register(pci_vgpu_render, sc);

	if (sc->fb_enabled) {
		// allocate legacy framebuffer bar
		err = pci_emul_alloc_bar(pi, LEGACY_CTRL_BAR, PCIBAR_MEM32,
		    DMEMSZ);
		assert(err == 0);
		err = pci_emul_alloc_bar(pi, LEGACY_FRAMEBUFFER_BAR,
		    PCIBAR_MEM32, FB_SIZE);
		assert(err == 0);
		pci_emul_set_bar_direct_mapped(pi, LEGACY_FRAMEBUFFER_BAR, 1);
	}

	console_set_hdpi(sc->hdpi_enabled);
	console_set_hardware_mouse(false);
	console_resize_register(resize_event, sc);

	return (0);
}

static void
pci_vgpu_baraddr(struct pci_devinst *pi, int baridx, int enabled,
    uint64_t address)
{
	struct pci_vgpu_softc *sc;
	int prot;
	uint32_t resource_id;
	struct vgpu_scanout *scanout;
	int stride;
	bool created;

	if (baridx != LEGACY_FRAMEBUFFER_BAR)
		return;

	sc = pi->pi_arg;
	if (!enabled)
		return;

	stride = roundup2(sc->resx * 4, 32);

	// create a default scanout
	resource_id = VGPU_LEGACY_RESOURCE_ID;
	scanout = pci_vgpu_find_resource(sc, resource_id);
	created = false;
	if (scanout == NULL) {
		if (!pci_vgpu_create_scanout(sc, resource_id, sc->resx,
			sc->resy, FB_SIZE, VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM)) {
			scanout = pci_vgpu_find_resource(sc, resource_id);
			created = true;
		}
	}

	if (scanout == NULL) {
		EPRINTLN("pci_vgpu: failed to create legacy framebuffer scanout");
		assert(0);
		return;
	}

	prot = PROT_READ | PROT_WRITE | PROT_DONT_ALLOCATE;
	if (vm_setup_memory_segment(pi->pi_vmctx, address, FB_SIZE, prot,
		(uintptr_t *)&scanout->base_ptr) != 0) {
		EPRINTLN("pci_vgpu: failed to direct-map legacy framebuffer "
		    "BAR at 0x%llx size 0x%lx", address, FB_SIZE);
		assert(0);
		return;
	}

	console_set_scanout(true, sc->resx, sc->resy, stride,
	    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, scanout->shm_name, FB_SIZE, true);
	if (created)
		memset((void *)scanout->base_ptr, 0, FB_SIZE);
}

static uint64_t
pci_vgpu_legacy_ctrl_read(struct pci_devinst *pi, int baridx, uint64_t offset,
    int size)
{
	struct pci_vgpu_softc *sc;
	uint64_t value;

	assert(baridx == 0);

	sc = pi->pi_arg;

	if (offset + size > DMEMSZ) {
		EPRINTLN(
		    "pci_vgpu_legacy_ctrl_read: read too large, offset %llu size %d",
		    offset, size);
		return (0);
	}

	value = 0;
	switch (offset) {
	case 0: // fbsize
		value = FB_SIZE;
		break;
	case 4: // width
		value = sc->resx;
		break;
	case 6: // height
		value = sc->resy;
		break;
	case 8: // depth
		value = 32;
		break;
	default:
		break;
	}

	DPRINTF((
	    "pci_vgpu_legacy_ctrl_read: offset 0x%llx, size: %d, value: 0x%llx",
	    offset, size, value));

	return (value);
}

uint64_t
pci_vgpu_barread(struct pci_devinst *pi, int baridx, uint64_t offset, int size)
{
	uint64_t val;

	switch (baridx) {
	case LEGACY_CTRL_BAR:
		val = pci_vgpu_legacy_ctrl_read(pi, baridx, offset, size);
		break;
	case LEGACY_FRAMEBUFFER_BAR:
		EPRINTLN("pci_vgpu_barread: can't read from the framebuffer");
		assert(0);
		break;
	default:
		val = vi_pci_read_modern(pi, baridx, offset, size);
		break;
	}

	return (val);
}

static void
pci_vgpu_legacy_ctrl_write(struct pci_devinst *pi, int baridx, uint64_t offset,
    int size, uint64_t value)
{
	struct pci_vgpu_softc *sc;

	sc = pi->pi_arg;

	assert(baridx == 0);

	DPRINTF((
	    "pci_vgpu_legacy_ctrl_write: offset 0x%llx, size: %d, value: 0x%llx",
	    offset, size, value));

	if (offset + size > DMEMSZ) {
		EPRINTLN(
		    "pci_vgpu_legacy_ctrl_write: write too large, offset %lld size %d",
		    offset, size);
		return;
	}

	switch (offset) {
	case 4:
		sc->resx = value;
		break;
	case 6:
		sc->resy = value;
		break;
	default:
		break;
	}
}

static void
pci_vgpu_barwrite(struct pci_devinst *pi, int baridx, uint64_t offset, int size,
    uint64_t value)
{
	switch (baridx) {
	case LEGACY_CTRL_BAR:
		pci_vgpu_legacy_ctrl_write(pi, baridx, offset, size, value);
		break;
	case LEGACY_FRAMEBUFFER_BAR:
		EPRINTLN("pci_vgpu_barwrite: can't write to the framebuffer");
		break;
	default:
		vi_pci_write_modern(pi, baridx, offset, size, value);
		break;
	}
}

static const struct pci_devemu pci_de_vgpu = {
	.pe_emu = "virtio-gpu",
	.pe_init = pci_vgpu_init,
	//.pe_legacy_config = pci_vgpu_legacy_config,
	.pe_barwrite = pci_vgpu_barwrite,
	.pe_barread = pci_vgpu_barread,
	.pe_baraddr = pci_vgpu_baraddr,
	.pe_reset = vi_pci_reset,
#ifdef BHYVE_SNAPSHOT
	.pe_snapshot = vi_pci_snapshot,
	.pe_pause = vi_pci_pause,
	.pe_resume = vi_pci_resume,
#endif
};
PCI_EMUL_SET(pci_de_vgpu);
