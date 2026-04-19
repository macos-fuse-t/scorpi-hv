/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Beckhoff Automation GmbH & Co. KG
 * Author: Corvin Kohne <c.koehne@beckhoff.com>
 */

#include <sys/types.h>

#include <support/endian.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <vmmapi.h>

#include "acpi_device.h"
#include "mem.h"
#include "tpm_crb.h"
#include "tpm_emul.h"
#include "tpm_intf.h"

#define TPM_CRB_INTF_NAME "crb"

struct tpm_crb_regs {
	union tpm_crb_reg_loc_state {
		struct {
			uint32_t tpm_established : 1;
			uint32_t loc_assigned : 1;
			uint32_t active_locality : 3;
			uint32_t _reserved : 2;
			uint32_t tpm_req_valid_sts : 1;
		};
		uint32_t val;
	} loc_state;	       /* 0h */
	uint8_t _reserved1[4]; /* 4h */
	union tpm_crb_reg_loc_ctrl {
		struct {
			uint32_t request_access : 1;
			uint32_t relinquish : 1;
			uint32_t seize : 1;
			uint32_t reset_establishment_bit : 1;
		};
		uint32_t val;
	} loc_ctrl; /* 8h */
	union tpm_crb_reg_loc_sts {
		struct {
			uint32_t granted : 1;
			uint32_t been_seized : 1;
		};
		uint32_t val;
	} loc_sts;		  /* Ch */
	uint8_t _reserved2[0x20]; /* 10h */
	union tpm_crb_reg_intf_id {
		struct {
			uint64_t interface_type : 4;
			uint64_t interface_version : 4;
			uint64_t cap_locality : 1;
			uint64_t cap_crb_idle_bypass : 1;
			uint64_t _reserved1 : 1;
			uint64_t cap_data_xfer_size_support : 2;
			uint64_t cap_fifo : 1;
			uint64_t cap_crb : 1;
			uint64_t _reserved2 : 2;
			uint64_t interface_selector : 2;
			uint64_t intf_sel_lock : 1;
			uint64_t _reserved3 : 4;
			uint64_t rid : 8;
			uint64_t vid : 16;
			uint64_t did : 16;
		};
		uint64_t val;
	} intf_id; /* 30h */
	union tpm_crb_reg_ctrl_ext {
		struct {
			uint32_t clear;
			uint32_t remaining_bytes;
		};
		uint64_t val;
	} ctrl_ext; /* 38h */
	union tpm_crb_reg_ctrl_req {
		struct {
			uint32_t cmd_ready : 1;
			uint32_t go_idle : 1;
		};
		uint32_t val;
	} ctrl_req; /* 40h */
	union tpm_crb_reg_ctrl_sts {
		struct {
			uint32_t tpm_sts : 1;
			uint32_t tpm_idle : 1;
		};
		uint32_t val;
	} ctrl_sts; /* 44h */
	union tpm_crb_reg_ctrl_cancel {
		struct {
			uint32_t cancel : 1;
		};
		uint32_t val;
	} ctrl_cancel; /* 48h */
	union tpm_crb_reg_ctrl_start {
		struct {
			uint32_t start : 1;
		};
		uint32_t val;
	} ctrl_start;				       /* 4Ch */
	uint32_t int_enable;			       /* 50h */
	uint32_t int_sts;			       /* 54h */
	uint32_t cmd_size;			       /* 58h */
	uint32_t cmd_addr_lo;			       /* 5Ch */
	uint32_t cmd_addr_hi;			       /* 60h */
	uint32_t rsp_size;			       /* 64h */
	uint64_t rsp_addr;			       /* 68h */
	uint8_t _reserved3[0x10];		       /* 70h */
	uint8_t data_buffer[TPM_CRB_DATA_BUFFER_SIZE]; /* 80h */
} __attribute__((packed));
static_assert(sizeof(struct tpm_crb_regs) == TPM_CRB_REGS_SIZE,
    "Invalid size of tpm_crb");

#define CRB_CMD_SIZE_READ(regs) (regs.cmd_size)
#define CRB_CMD_SIZE_WRITE(regs, val) \
	do {                          \
		regs.cmd_size = val;  \
	} while (0)
#define CRB_CMD_ADDR_READ(regs) \
	(((uint64_t)regs.cmd_addr_hi << 32) | regs.cmd_addr_lo)
#define CRB_CMD_ADDR_WRITE(regs, val)                \
	do {                                         \
		regs.cmd_addr_lo = (uint64_t)(val) & 0xFFFFFFFF; \
		regs.cmd_addr_hi = (uint64_t)(val) >> 32;        \
	} while (0)
#define CRB_RSP_SIZE_READ(regs) (regs.rsp_size)
#define CRB_RSP_SIZE_WRITE(regs, val) \
	do {                          \
		regs.rsp_size = val;  \
	} while (0)
#define CRB_RSP_ADDR_READ(regs) (regs.rsp_addr)
#define CRB_RSP_ADDR_WRITE(regs, val) \
	do {                          \
		regs.rsp_addr = val;  \
	} while (0)

struct tpm_cmd_hdr {
	uint16_t tag;
	uint32_t len;
	union {
		uint32_t ordinal;
		uint32_t errcode;
	};
} __attribute__((packed));

struct tpm_crb {
	struct tpm_emul *emul;
	void *emul_sc;
	struct tpm_crb_regs regs;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool thread_created;
	bool cond_initialized;
	bool mutex_initialized;
	bool mmio_registered;
	bool closing;
};

static void *
tpm_crb_thread(void *arg)
{
	struct tpm_crb *crb;

	crb = arg;

#ifdef __APPLE__
	pthread_setname_np("tpm_intf_crb");
#endif

	pthread_mutex_lock(&crb->mutex);
	for (;;) {
		uint64_t cmd_addr, rsp_addr;
		uint32_t cmd_size, rsp_size;
		uint64_t cmd_off, rsp_off;
		uint8_t cmd[TPM_CRB_DATA_BUFFER_SIZE];
		uint8_t rsp[TPM_CRB_DATA_BUFFER_SIZE];
		struct tpm_cmd_hdr *req;
		int error;

		if (crb->closing)
			break;

		pthread_cond_wait(&crb->cond, &crb->mutex);

		if (crb->closing)
			break;

		cmd_addr = CRB_CMD_ADDR_READ(crb->regs);
		rsp_addr = CRB_RSP_ADDR_READ(crb->regs);
		cmd_size = CRB_CMD_SIZE_READ(crb->regs);
		rsp_size = CRB_RSP_SIZE_READ(crb->regs);

		if (cmd_addr < TPM_CRB_DATA_BUFFER_ADDRESS ||
		    cmd_size < sizeof(struct tpm_cmd_hdr) ||
		    cmd_size > TPM_CRB_DATA_BUFFER_SIZE ||
		    cmd_addr + cmd_size >
		    TPM_CRB_DATA_BUFFER_ADDRESS + TPM_CRB_DATA_BUFFER_SIZE) {
			warnx("%s: invalid command buffer %#llx/%#x",
			    __func__, (unsigned long long)cmd_addr, cmd_size);
			crb->regs.ctrl_start.start = false;
			continue;
		}

		if (rsp_addr < TPM_CRB_DATA_BUFFER_ADDRESS ||
		    rsp_size < sizeof(struct tpm_cmd_hdr) ||
		    rsp_size > TPM_CRB_DATA_BUFFER_SIZE ||
		    rsp_addr + rsp_size >
		    TPM_CRB_DATA_BUFFER_ADDRESS + TPM_CRB_DATA_BUFFER_SIZE) {
			warnx("%s: invalid response buffer %#llx/%#x",
			    __func__, (unsigned long long)rsp_addr, rsp_size);
			crb->regs.ctrl_start.start = false;
			continue;
		}

		cmd_off = cmd_addr - TPM_CRB_DATA_BUFFER_ADDRESS;
		rsp_off = rsp_addr - TPM_CRB_DATA_BUFFER_ADDRESS;

		memcpy(cmd, crb->regs.data_buffer, sizeof(cmd));
		req = (struct tpm_cmd_hdr *)&cmd[cmd_off];
		if (be32toh(req->len) < sizeof(struct tpm_cmd_hdr) ||
		    be32toh(req->len) > cmd_size) {
			warnx("%s: invalid TPM request header", __func__);
			crb->regs.ctrl_start.start = false;
			continue;
		}

		pthread_mutex_unlock(&crb->mutex);

		memset(rsp, 0, sizeof(rsp));
		error = crb->emul->execute_cmd(crb->emul_sc, req,
		    be32toh(req->len), &rsp[rsp_off], rsp_size);

		pthread_mutex_lock(&crb->mutex);
		if (error != 0) {
			warnx("%s: cmd 0x%08x len %u failed: %d", __func__,
			    be32toh(req->ordinal), be32toh(req->len), error);
		}
		memset(crb->regs.data_buffer, 0, sizeof(crb->regs.data_buffer));
		memcpy(&crb->regs.data_buffer[rsp_off], &rsp[rsp_off], rsp_size);
		crb->regs.ctrl_start.start = false;
	}
	pthread_mutex_unlock(&crb->mutex);

	return (NULL);
}

static int
tpm_crb_mmiocpy(void *dst, const void *src, int size)
{
	if (!(size == 1 || size == 2 || size == 4 || size == 8))
		return (EINVAL);
	memcpy(dst, src, size);

	return (0);
}

static int
tpm_crb_mem_handler(struct vcpu *vcpu __unused, int dir, uint64_t addr,
    int size, uint64_t *val, void *arg1, long arg2 __unused)
{
	struct tpm_crb *crb;
	uint8_t *ptr;
	uint64_t off, shift;
	int error;

	if ((addr & (size - 1)) != 0) {
		warnx("%s: unaligned %s access @ %#llx [size = %x]", __func__,
		    dir == MEM_F_READ ? "read" : "write",
		    (unsigned long long)addr, size);
		return (EINVAL);
	}

	crb = arg1;
	error = 0;
	off = addr - TPM_CRB_ADDRESS;
	if (off >= TPM_CRB_REGS_SIZE || off + size > TPM_CRB_REGS_SIZE)
		return (EINVAL);

	shift = 8 * (off & 3);
	ptr = (uint8_t *)&crb->regs + off;

	if (dir == MEM_F_READ) {
		*val = 0;
		return (tpm_crb_mmiocpy(val, ptr, size));
	}

	switch (off & ~0x3) {
	case offsetof(struct tpm_crb_regs, loc_ctrl): {
		union tpm_crb_reg_loc_ctrl loc_ctrl;

		if ((size_t)size > sizeof(loc_ctrl))
			goto err_out;

		*val = *val << shift;
		tpm_crb_mmiocpy(&loc_ctrl, val, size);

		if (loc_ctrl.relinquish) {
			crb->regs.loc_sts.granted = false;
			crb->regs.loc_state.loc_assigned = false;
		} else if (loc_ctrl.request_access) {
			crb->regs.loc_sts.granted = true;
			crb->regs.loc_state.loc_assigned = true;
		}
		break;
	}
	case offsetof(struct tpm_crb_regs, ctrl_req): {
		union tpm_crb_reg_ctrl_req req;

		if ((size_t)size > sizeof(req))
			goto err_out;

		*val = *val << shift;
		tpm_crb_mmiocpy(&req, val, size);

		if (req.cmd_ready && !req.go_idle)
			crb->regs.ctrl_sts.tpm_idle = false;
		else if (!req.cmd_ready && req.go_idle)
			crb->regs.ctrl_sts.tpm_idle = true;
		break;
	}
	case offsetof(struct tpm_crb_regs, ctrl_cancel):
		/*
		 * CRB CTRL_CANCEL is a write-only doorbell. A write of zero is
		 * ignored, and a write of one only matters while CTRL_START is
		 * still set. The swtpm command socket used here has no separate
		 * control channel for asynchronous cancellation, so accept the
		 * request as a no-op instead of warning during normal guest
		 * probing and timeout cleanup paths.
		 */
		break;
	case offsetof(struct tpm_crb_regs, int_enable):
		break;
	case offsetof(struct tpm_crb_regs, ctrl_start): {
		union tpm_crb_reg_ctrl_start start;

		if ((size_t)size > sizeof(start))
			goto err_out;

		*val = *val << shift;

		pthread_mutex_lock(&crb->mutex);
		tpm_crb_mmiocpy(&start, val, size);
		if (!start.start || crb->regs.ctrl_start.start) {
			pthread_mutex_unlock(&crb->mutex);
			break;
		}

		crb->regs.ctrl_start.start = true;
		pthread_cond_signal(&crb->cond);
		pthread_mutex_unlock(&crb->mutex);
		break;
	}
	case offsetof(struct tpm_crb_regs, cmd_size):
	case offsetof(struct tpm_crb_regs, cmd_addr_lo):
	case offsetof(struct tpm_crb_regs, cmd_addr_hi):
	case offsetof(struct tpm_crb_regs, rsp_size):
	case offsetof(struct tpm_crb_regs, rsp_addr) ...
	    offsetof(struct tpm_crb_regs, rsp_addr) + 4:
	case offsetof(struct tpm_crb_regs, data_buffer) ...
	    offsetof(struct tpm_crb_regs, data_buffer) +
	    TPM_CRB_DATA_BUFFER_SIZE - 1:
		pthread_mutex_lock(&crb->mutex);
		error = tpm_crb_mmiocpy(ptr, val, size);
		pthread_mutex_unlock(&crb->mutex);
		if (error)
			goto err_out;
		break;
	default:
		error = EINVAL;
		goto err_out;
	}

	return (0);

err_out:
	warnx("%s: invalid %s @ %#llx [size = %d]", __func__,
	    dir == MEM_F_READ ? "read" : "write", (unsigned long long)addr,
	    size);

	return (error);
}

static int
tpm_crb_modify_mmio_registration(bool registration, void *arg1)
{
	struct mem_range crb_mmio = {
		.name = "crb-mmio",
		.base = TPM_CRB_ADDRESS,
		.size = TPM_CRB_MMIO_SIZE,
		.flags = MEM_F_RW,
		.arg1 = arg1,
		.handler = tpm_crb_mem_handler,
	};

	if (registration)
		return (register_mem(&crb_mmio));

	return (unregister_mem(&crb_mmio));
}

static int
tpm_crb_init(void **sc, struct tpm_emul *emul, void *emul_sc,
    struct acpi_device *acpi_dev)
{
	struct tpm_crb *crb;
	int error;

	assert(sc != NULL);
	assert(emul != NULL);

	crb = calloc(1, sizeof(*crb));
	if (crb == NULL)
		return (ENOMEM);

	crb->emul = emul;
	crb->emul_sc = emul_sc;

	crb->regs.loc_state.tpm_req_valid_sts = true;
	crb->regs.intf_id.interface_type = TPM_INTF_TYPE_CRB;
	crb->regs.intf_id.interface_version = TPM_INTF_VERSION_CRB;
	crb->regs.intf_id.cap_data_xfer_size_support =
	    TPM_INTF_CAP_CRB_DATA_XFER_SIZE_64;
	crb->regs.intf_id.cap_crb = true;
	crb->regs.intf_id.interface_selector = TPM_INTF_SELECTOR_CRB;
	crb->regs.intf_id.vid = 0x1014; /* IBM */
	crb->regs.ctrl_sts.tpm_idle = true;

	CRB_CMD_SIZE_WRITE(crb->regs, TPM_CRB_DATA_BUFFER_SIZE);
	CRB_CMD_ADDR_WRITE(crb->regs, TPM_CRB_DATA_BUFFER_ADDRESS);
	CRB_RSP_SIZE_WRITE(crb->regs, TPM_CRB_DATA_BUFFER_SIZE);
	CRB_RSP_ADDR_WRITE(crb->regs, TPM_CRB_DATA_BUFFER_ADDRESS);

	error = acpi_device_add_res_fixed_memory32(acpi_dev, false,
	    TPM_CRB_ADDRESS, TPM_CRB_CONTROL_AREA_SIZE);
	if (error)
		goto err_out;

	error = tpm_crb_modify_mmio_registration(true, crb);
	if (error) {
		warnc(error, "%s: failed to register crb mmio", __func__);
		goto err_out;
	}
	crb->mmio_registered = true;

	error = pthread_mutex_init(&crb->mutex, NULL);
	if (error) {
		warnc(error, "%s: failed to init mutex", __func__);
		goto err_out;
	}
	crb->mutex_initialized = true;

	error = pthread_cond_init(&crb->cond, NULL);
	if (error) {
		warnc(error, "%s: failed to init cond", __func__);
		goto err_out;
	}
	crb->cond_initialized = true;

	error = pthread_create(&crb->thread, NULL, tpm_crb_thread, crb);
	if (error) {
		warnc(error, "%s: failed to create thread", __func__);
		goto err_out;
	}
	crb->thread_created = true;

#if defined(__FreeBSD__)
	pthread_set_name_np(crb->thread, "tpm_intf_crb");
#elif defined(__linux__)
	pthread_setname_np(crb->thread, "tpm_intf_crb");
#endif

	*sc = crb;

	return (0);

err_out:
	if (crb->thread_created) {
		crb->closing = true;
		pthread_cond_signal(&crb->cond);
		pthread_join(crb->thread, NULL);
	}
	if (crb->cond_initialized)
		pthread_cond_destroy(&crb->cond);
	if (crb->mutex_initialized)
		pthread_mutex_destroy(&crb->mutex);
	if (crb->mmio_registered)
		(void)tpm_crb_modify_mmio_registration(false, NULL);
	free(crb);

	return (error);
}

static void
tpm_crb_deinit(void *sc)
{
	struct tpm_crb *crb;

	crb = sc;
	if (crb == NULL)
		return;

	pthread_mutex_lock(&crb->mutex);
	crb->closing = true;
	pthread_cond_signal(&crb->cond);
	pthread_mutex_unlock(&crb->mutex);

	pthread_join(crb->thread, NULL);
	pthread_cond_destroy(&crb->cond);
	pthread_mutex_destroy(&crb->mutex);

	(void)tpm_crb_modify_mmio_registration(false, NULL);

	free(crb);
}

static struct tpm_intf tpm_intf_crb = {
	.name = TPM_CRB_INTF_NAME,
	.init = tpm_crb_init,
	.deinit = tpm_crb_deinit,
};
TPM_INTF_SET(tpm_intf_crb);
