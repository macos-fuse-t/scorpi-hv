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

#define KB	(1024UL)
#define MB	(1024 * 1024UL)
#define FB_SIZE (128 * MB)

static int vgpu_debug = 0;
#define DPRINTF(params) \
	if (vgpu_debug) \
	PRINTLN params
#define WPRINTF(params)	       PRINTLN params

#define VQ_MAX_DESC	       32

#define VGPU_MAXQ	       3
#define VGPU_RINGSZ	       1024

#define VGPU_CTRL	       0
#define VGPU_CURSOR	       1

#define VGPU_S_HOSTCAPS	       (VIRTIO_RING_F_INDIRECT_DESC)

#define COLS_MAX	       3840
#define ROWS_MAX	       2160

#define COLS_DEFAULT	       1024
#define ROWS_DEFAULT	       768

#define COLS_MIN	       640
#define ROWS_MIN	       480

#define LEGACY_FRAMEBUFFER_BAR 1
#define LEGACY_CTRL_BAR	       0

#define DMEMSZ		       512

#define FB_SIZE		       (128 * MB)

#define COLS_MAX	       3840
#define ROWS_MAX	       2160

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

	bool hdpi;
	bool fb_enabled; /* enable legacy framebuffer */

	LIST_HEAD(scanouts, vgpu_scanout) scanouts;
};

extern uuid_t vm_uuid;

static void pci_vgpu_reset(void *vsc);
static int pci_vgpu_cfgread(void *vsc, int offset, int size, uint32_t *retval);
static int pci_vgpu_cfgwrite(void *vsc, int offset, int size, uint32_t value);
static void pci_vgpu_neg_features(void *vsc, uint64_t negotiated_features);

static struct virtio_consts vgpu_vi_consts = {
	.vc_name = "vgpu",
	.vc_nvq = VGPU_MAXQ - 1,
	.vc_cfgsize = sizeof(struct virtio_gpu_config),
	.vc_reset = pci_vgpu_reset,
	.vc_cfgread = pci_vgpu_cfgread,
	.vc_cfgwrite = pci_vgpu_cfgwrite,
	.vc_apply_features = pci_vgpu_neg_features,
	.vc_hv_caps = VGPU_S_HOSTCAPS | VIRTIO_F_VERSION_1,
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
		sc->vsc_config.events_read = 0;
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

static void
pci_vgpu_reset(void *vsc)
{
	struct pci_vgpu_softc *sc = vsc;
	struct vgpu_scanout *scanout, *tmp;

	DPRINTF(("vgpu: device reset requested !"));
	if (!sc->fb_enabled) {
		console_set_scanout(false, 0, 0, 0, 0, NULL, 0, false);

		LIST_FOREACH_SAFE(scanout, &sc->scanouts, entries, tmp) {
			LIST_REMOVE(scanout, entries);
			munmap(scanout->base_ptr, scanout->size);
			shm_unlink(scanout->shm_name);
			close(scanout->shm_fd);
			free(scanout);
		}
	}

	sc->resx = sc->start_resx;
	sc->resy = sc->start_resy;
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

	assert(rsp_len == sizeof(di));
	len = MIN(rsp_len, sizeof(di));
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

	scanout->shm_fd = shm_open(scanout->shm_name, O_CREAT | O_RDWR,
	    S_IRUSR | S_IWUSR);
	if (scanout->shm_fd == -1) {
		EPRINTLN("shm_open %s", scanout->shm_name);
		free(scanout);
		return (-1);
	}

	// Resize the shared memory
	if (ftruncate(scanout->shm_fd, sc_size) == -1) {
		EPRINTLN("ftruncate %s, size %u", scanout->shm_name, sc_size);
		close(scanout->shm_fd);
		shm_unlink(scanout->shm_name);
		free(scanout);
		return (-1);
	}

	scanout->base_ptr = mmap(NULL, sc_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, scanout->shm_fd, 0);
	if (scanout->base_ptr == MAP_FAILED) {
		EPRINTLN("mmap");
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

	assert(rsp_len == sizeof(hdr));
	len = MIN(rsp_len, sizeof(hdr));
	memcpy(rsp, &hdr, len);

	DPRINTF(("create_resource_2d %d, w %d h %d", le32toh(res->resource_id),
	    le32toh(res->width), le32toh(res->height)));
	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (scanout) {
		// already exists
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return len;
	}

	sc_size = roundup2(le32toh(res->width) * 4, 32) * le32toh(res->height);
	if (pci_vgpu_create_scanout(sc, le32toh(res->resource_id),
		le32toh(res->width), le32toh(res->height), sc_size,
		le32toh(res->format)) != 0) {
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
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

	assert(rsp_len == sizeof(hdr));
	len = MIN(rsp_len, sizeof(hdr));
	memcpy(rsp, &hdr, len);

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return (len);
	}

	LIST_REMOVE(scanout, entries);
	munmap(scanout->base_ptr, scanout->size);
	shm_unlink(scanout->shm_name);
	close(scanout->shm_fd);
	free(scanout);

	return (len);
}

static void
pci_vgpu_render(void *arg)
{
}

static size_t
pci_vgpu_resource_attach_backing(struct pci_vgpu_softc *sc,
    struct virtio_gpu_ctrl_hdr *req, uint8_t *rsp, size_t rsp_len)
{
	struct virtio_gpu_resource_attach_backing *res;
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_mem_entry *mem;
	size_t len;
	int i;
	struct vgpu_scanout *scanout;

	res = (struct virtio_gpu_resource_attach_backing *)req;
	DPRINTF(("resource_attach_backing: %d, res %d", res->nr_entries,
	    le32toh(res->resource_id)));

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	assert(rsp_len == sizeof(hdr));
	len = MIN(rsp_len, sizeof(hdr));
	memcpy(rsp, &hdr, len);

	if (!res->nr_entries)
		return len;

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		return len;
	}

	scanout->backing_iov = calloc(le32toh(res->nr_entries),
	    sizeof(struct iovec));
	scanout->iov_cnt = le32toh(res->nr_entries);

	// map guest pages into host contiguos memorry
	mem = (struct virtio_gpu_mem_entry *)(res + 1);
	for (i = 0; i < res->nr_entries; i++) {
		scanout->backing_iov[i].iov_base = vm_map_gpa(
		    sc->vsc_vs.vs_pi->pi_vmctx, mem[i].addr, mem[i].length);
		scanout->backing_iov[i].iov_len = mem[i].length;

		assert(scanout->backing_iov[i].iov_base);
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

	assert(rsp_len == sizeof(hdr));
	len = MIN(rsp_len, sizeof(hdr));
	memcpy(rsp, &hdr, len);

	if (le32toh(res->resource_id)) {
		scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
		if (!scanout) {
			EPRINTLN("pci_vgpu: scanout %d not found",
			    le32toh(res->resource_id));
			hdr.type = htole32(
			    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
			return (len);
		}
		scanout->width = le32toh(res->r.width);
		scanout->height = le32toh(res->r.height);

		console_set_scanout(true, scanout->width, scanout->height,
		    scanout->stride, scanout->format, scanout->shm_name,
		    scanout->size, false);
		DPRINTF(("active scanout: %d: %dx%d, %d", scanout->resource_id,
		    scanout->width, scanout->height, scanout->iov_cnt));
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

	assert(rsp_len == sizeof(hdr));
	len = MIN(rsp_len, sizeof(hdr));
	memcpy(rsp, &hdr, len);

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return (len);
	}

	scanout->iov_cnt = 0;
	free(scanout->backing_iov);

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

	assert(rsp_len == sizeof(hdr));
	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);
	len = MIN(rsp_len, sizeof(hdr));

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
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
	size_t src_off, dst_off, lw;

	res = (struct virtio_gpu_transfer_to_host_2d *)req;
	if (res->r.x != 0 || res->r.y != 0 || res->r.width != sc->resx ||
	    res->r.height != sc->resy) {
		DPRINTF(("transfer_to_host_2d %d %d %d %d", le32toh(res->r.x),
		    le32toh(res->r.y), le32toh(res->r.width),
		    le32toh(res->r.height)));
	}

	pci_vgpu_prepare_response(req, &hdr, VIRTIO_GPU_RESP_OK_NODATA);

	len = MIN(rsp_len, sizeof(hdr));
	memcpy(rsp, &hdr, len);

	scanout = pci_vgpu_find_resource(sc, le32toh(res->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(res->resource_id));
		hdr.type = htole32(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return (len);
	}

	lw = scanout->stride;
	if (le32toh(res->r.x) != 0 || le32toh(res->r.width) != scanout->width) {
		for (h = 0; h < le32toh(res->r.height); h++) {
			src_off = le64toh(res->offset) + lw * h;
			dst_off = lw * (le32toh(res->r.y) + h) +
			    le32toh(res->r.x) * 4;
			iov_copy(scanout->base_ptr + dst_off,
			    le32toh(res->r.width) * 4, scanout->backing_iov,
			    scanout->iov_cnt, src_off);
		}
	} else {
		src_off = le64toh(res->offset);
		dst_off = le32toh(res->r.y) * lw;
		iov_copy(scanout->base_ptr + dst_off,
		    le32toh(res->r.height) * lw, scanout->backing_iov,
		    scanout->iov_cnt, src_off);
	}
	return (len);
}

static int pci_vgpu_generate_edid(struct pci_vgpu_softc *sc, uint16_t width,
    uint16_t height, struct edid *edid);

__unused static size_t
pci_vgpu_get_edid(struct pci_vgpu_softc *sc, struct virtio_gpu_ctrl_hdr *req,
    uint8_t *rsp, size_t rsp_len)
{
	// struct virtio_gpu_cmd_get_edid *cmd = (struct virtio_gpu_cmd_get_edid
	// *)req;
	struct virtio_gpu_resp_edid edid;
	// struct vgpu_scanout *scanout;
	size_t len, size;

	bzero(&edid, sizeof(edid));
	pci_vgpu_prepare_response(req, &edid.hdr, VIRTIO_GPU_RESP_OK_EDID);
	/*scanout = pci_vgpu_find_resource(sc, le32toh(cmd->scanout));
	if (!scanout) {
	    EPRINTLN("pci_vgpu_get_edid: scanout %d not found",
	le32toh(cmd->scanout)); exit(-1); return (0);
	}*/

	size = pci_vgpu_generate_edid(sc, 1024, 768, (struct edid *)&edid.edid);
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

	len = MIN(rsp_len, sizeof(hdr));
	memcpy(rsp, &hdr, len);

	return (len);
}

static size_t
pci_vgpu_process_cmd(struct pci_vgpu_softc *sc, struct virtio_gpu_ctrl_hdr *req,
    uint8_t *rsp, size_t rsp_len)
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
		len = pci_vgpu_resource_attach_backing(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
		len = pci_vgpu_resource_dettach_backing(sc, req, rsp, rsp_len);
		break;
	case VIRTIO_GPU_CMD_GET_EDID:
		len = pci_vgpu_default_response(sc, req, rsp, rsp_len,
		    VIRTIO_GPU_RESP_ERR_UNSPEC);
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
	struct virtio_gpu_ctrl_hdr *cmd;

	// DPRINTF(("vgpu: pci_vgpu_ping_ctrl"));
	pthread_mutex_lock(&sc->vsc_mtx);
	// vq_kick_disable(vq);

	while (vq_has_descs(vq)) {
		int n = vq_getchain(vq, iov, VQ_MAX_DESC, &req);
		assert(req.writable == 1);
		assert(n < VQ_MAX_DESC);
		assert(n == req.readable + req.writable);
		assert(req.writable <= 1);

		cmd = NULL;
		if (iov_to_buf(iov, req.readable, (void **)&cmd) > 0) {
			rsp_len = pci_vgpu_process_cmd(sc, cmd,
			    iov[req.readable].iov_base,
			    iov[req.readable].iov_len);
			free(cmd);
		}
		assert(rsp_len > 0);
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

	if (!req->resource_id) {
		console_set_mouse_scanout(false, 0, 0, 0, 0, 0, NULL);
		return (0);
	}

	scanout = pci_vgpu_find_resource(sc, le32toh(req->resource_id));
	if (!scanout) {
		EPRINTLN("pci_vgpu: scanout %d not found",
		    le32toh(req->resource_id));
		return (0);
	}

	console_set_mouse_scanout(true, le32toh(scanout->width),
	    le32toh(scanout->height), le32toh(scanout->format),
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

static int
pci_vgpu_generate_edid(struct pci_vgpu_softc *sc, uint16_t width,
    uint16_t height, struct edid *edid)
{
	uint8_t width_cm = width / 10;
	uint8_t height_cm = height / 10;

	memset(edid, 0, sizeof(struct edid)); // Clear EDID struct
	*edid = (struct edid) { .header = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
				    0xFF, 0x00 },
		.mfg_id = { 'S', 'C' },
		.prod_code = { 0x01, 0x00 }, //  product code
		.serial = { 0, 0, 0, 0 },    //  4-byte serial number
		.mfg_week = 0x01,	     // Week 1
		.mfg_year = 0x23,	     // Year 2025 (0x23 = 2025 - 1990)
		.version = 0x01,	     // EDID version 1
		.revision = 0x03,	     // EDID revision 3
		.input = 0x80,		     // Digital display
		.width_cm = width_cm,	     // Physical width in cm
		.height_cm = height_cm,	     // Physical height in cm
		.gamma = 120,	  // Gamma (2.2, computed as (gamma - 1) * 100)
		.features = 0x02, // Default color, preferred timing mode
		.red_green_lo = 0x20,
		.black_white_lo = 0x20,
		.red_x = 0xA4,
		.red_y = 0x56,
		.green_x = 0x34,
		.green_y = 0x78,
		.blue_x = 0x12,
		.blue_y = 0x9A,
		.white_x = 0xC8,
		.white_y = 0x32,
		.extensions = 0x00, // No extensions
		.checksum = 0x00 };

	uint8_t sum = 0;
	for (size_t i = 0; i < 127; i++) {
		sum += ((uint8_t *)edid)[i];
	}
	edid->checksum = (256 - sum) % 256;

	return (sizeof(*edid));
}

static void
resize_event(int x, int y, void *arg)
{
	struct pci_vgpu_softc *sc;

	sc = (struct pci_vgpu_softc *)arg;
	pthread_mutex_lock(&sc->vsc_mtx);

	sc = arg;
	DPRINTF(("resize_event %d %d", x, y));
	if (!sc->fb_enabled && x >= COLS_MIN && x <= COLS_MAX &&
	    y >= ROWS_MIN && y <= ROWS_MAX) {
		sc->resx = x;
		sc->resy = y;
		sc->vsc_config.events_read = 1;
		sc->vsc_vs.vs_isr |= 2;
		if (sc->vsc_vs.vs_queues[VGPU_CTRL].vq_enabled)
			vq_interrupt(&sc->vsc_vs,
			    &sc->vsc_vs.vs_queues[VGPU_CTRL]);
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

	value = get_config_value_node(nvl, "fb");
	if (value && !strcmp(value, "on"))
		sc->fb_enabled = true;

	value = get_config_value_node(nvl, "hdpi");
	if (value && !strcmp(value, "on"))
		sc->hdpi = true;

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
		sc->resx = strtol(value, NULL, 10);
	else
		sc->resx = COLS_DEFAULT;
	sc->start_resx = sc->resx;

	value = get_config_value_node(nvl, "h");
	if (value)
		sc->resy = strtol(value, NULL, 10);
	else
		sc->resy = ROWS_DEFAULT;
	sc->start_resy = sc->resy;

	if (sc->resx > COLS_MAX || sc->resy > ROWS_MAX) {
		EPRINTLN("fbuf: max resolution is %ux%u", COLS_MAX, ROWS_MAX);
		return (-1);
	}
	if (sc->resx < COLS_MIN || sc->resy < ROWS_MIN) {
		EPRINTLN("vgpu: minimum resolution is %ux%u", COLS_MIN,
		    ROWS_MIN);
		return (-1);
	}

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
	}

	console_set_hdpi(sc->hdpi);

	console_resize_register(resize_event, sc);

	return (0);
}

static void
pci_vgpu_baraddr(struct pci_devinst *pi, int baridx, int enabled,
    uint64_t address)
{
	static int once = 0;
	struct pci_vgpu_softc *sc;
	int prot;
	uint32_t resource_id;
	struct vgpu_scanout *scanout;
	int stride;

	if (baridx != LEGACY_FRAMEBUFFER_BAR)
		return;

	sc = pi->pi_arg;
	if (!enabled) {
		// console_set_scanout(false, 0, 0, 0, NULL, false);
	} else {
		stride = roundup2(sc->resx * 4, 32);
		if (once) {
			return;
		}
		once++;

		// create a default scanout
		resource_id = 0xFFFFFFFF;
		if (!pci_vgpu_create_scanout(sc, resource_id, sc->resx,
			sc->resy, FB_SIZE, VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM)) {
			scanout = pci_vgpu_find_resource(sc, resource_id);

			prot = PROT_READ | PROT_WRITE | PROT_DONT_ALLOCATE;
			if (vm_setup_memory_segment(pi->pi_vmctx, address,
				FB_SIZE, prot,
				(uintptr_t *)&scanout->base_ptr)) {
				EPRINTLN(
				    "pci_fbuf: vm_setup_memory_segment() failed");
			}

			console_set_scanout(true, sc->resx, sc->resy, stride,
			    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, scanout->shm_name,
			    FB_SIZE, true);
			memset((void *)scanout->base_ptr, 0, FB_SIZE);
		}
	}
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
