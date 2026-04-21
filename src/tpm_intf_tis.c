/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/types.h>

#include <support/endian.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vmmapi.h>

#include "acpi_device.h"
#include "mem.h"
#include "tpm_emul.h"
#include "tpm_intf.h"
#include "tpm_tis.h"

#ifndef BIT
#define BIT(x) (1u << (x))
#endif

#define TPM_INTF_TIS_NAME		"tis"

#define TPM_NO_LOCALITY			0xff

#define TPM_ACCESS			0x00
#define TPM_INT_ENABLE		0x08
#define TPM_INT_VECTOR		0x0c
#define TPM_INT_STATUS		0x10
#define TPM_INTF_CAPABILITY		0x14
#define TPM_STS			0x18
#define TPM_DATA_FIFO		0x24
#define TPM_INTERFACE_ID		0x30
#define TPM_XDATA_FIFO		0x80
#define TPM_XDATA_FIFO_END		0xbc
#define TPM_DID_VID			0xf00
#define TPM_RID			0xf04

#define TPM_STS_TPM20			BIT(26)
#define TPM_STS_CMD_CANCEL		BIT(24)
#define TPM_STS_VALID			BIT(7)
#define TPM_STS_CMD_RDY			BIT(6)
#define TPM_STS_CMD_START		BIT(5)
#define TPM_STS_DATA_AVAIL		BIT(4)
#define TPM_STS_DATA_EXPECTED		BIT(3)
#define TPM_STS_RESP_RETRY		BIT(1)

#define TPM_STS_BURST_COUNT_SHIFT	8
#define TPM_STS_BURST_COUNT(x)		((x) << TPM_STS_BURST_COUNT_SHIFT)

#define TPM_ACCESS_TPM_REG_VALID_STS		BIT(7)
#define TPM_ACCESS_ACTIVE_LOCALITY	BIT(5)
#define TPM_ACCESS_BEEN_SEIZED		BIT(4)
#define TPM_ACCESS_SEIZE		BIT(3)
#define TPM_ACCESS_PENDING_REQUEST	BIT(2)
#define TPM_ACCESS_REQUEST_USE		BIT(1)
#define TPM_ACCESS_TPM_ESTABLISHMENT	BIT(0)

#define TPM_INT_ENABLED			BIT(31)
#define TPM_INT_DATA_AVAIL		BIT(0)
#define TPM_INT_STS_VALID		BIT(1)
#define TPM_INT_LOCALITY_CHANGED	BIT(2)
#define TPM_INT_CMD_RDY			BIT(7)
#define TPM_INT_POLARITY_MASK		(3u << 3)
#define TPM_INT_POLARITY_LOW_LEVEL	BIT(3)
#define TPM_INT_SUPPORTED						\
	(TPM_INT_LOCALITY_CHANGED | TPM_INT_DATA_AVAIL |		\
	    TPM_INT_STS_VALID | TPM_INT_CMD_RDY)

#define TPM_INTF_CAPS_TPM20			0x30000000
#define TPM_INTF_CAPS_DATA_TRANSFER_64B		(3u << 9)
#define TPM_INTF_CAPS_BURST_COUNT_DYNAMIC	(0u << 8)
#define TPM_INTF_CAPS_INTERRUPT_LOW_LEVEL	BIT(4)
#define TPM_INTF_CAPS_SUPPORTED					\
	(TPM_INTF_CAPS_INTERRUPT_LOW_LEVEL |			\
	    TPM_INTF_CAPS_BURST_COUNT_DYNAMIC |			\
	    TPM_INTF_CAPS_DATA_TRANSFER_64B | TPM_INTF_CAPS_TPM20 |	\
	    TPM_INT_SUPPORTED)

#define TPM_INTF_ID_INTERFACE_FIFO	0x0
#define TPM_INTF_ID_INTERFACE_VER_FIFO	(0u << 4)
#define TPM_INTF_ID_CAP_5_LOCALITIES	BIT(8)
#define TPM_INTF_ID_CAP_TIS_SUPPORTED	BIT(13)
#define TPM_INTF_ID_INT_SEL_LOCK	BIT(19)
#define TPM_INTF_ID_SUPPORTED_FLAGS				\
	(TPM_INTF_ID_INTERFACE_FIFO | TPM_INTF_ID_INTERFACE_VER_FIFO | \
	    TPM_INTF_ID_CAP_5_LOCALITIES |				\
	    TPM_INTF_ID_CAP_TIS_SUPPORTED)

#define TPM_DID_VALUE			0x0001
#define TPM_VID_VALUE			0x1014
#define TPM_RID_VALUE			0x0001

#define TPM_NO_DATA_BYTE		0xff

enum tpm_tis_state {
	TPM_STATE_IDLE = 0,
	TPM_STATE_READY,
	TPM_STATE_COMPLETION,
	TPM_STATE_EXECUTION,
	TPM_STATE_RECEPTION,
};

struct tpm_tis_locality {
	enum tpm_tis_state state;
	uint8_t access;
	uint32_t sts;
	uint32_t iface_id;
	uint32_t int_enable;
	uint32_t int_status;
};

struct tpm_tis {
	struct tpm_emul *emul;
	void *emul_sc;
	struct tpm_tis_locality loc[TPM_TIS_NUM_LOCALITIES];
	uint8_t buffer[TPM_TIS_BUFFER_SIZE];
	uint32_t rw_offset;
	uint8_t active_locality;
	bool mmio_registered;
	pthread_mutex_t mutex;
	bool mutex_initialized;
};

struct tpm_cmd_hdr {
	uint16_t tag;
	uint32_t len;
	union {
		uint32_t ordinal;
		uint32_t errcode;
	};
} __attribute__((packed));

static uint32_t
tpm_tis_cmd_size(const uint8_t *buf)
{
	const struct tpm_cmd_hdr *hdr;

	hdr = (const struct tpm_cmd_hdr *)buf;
	return (be32toh(hdr->len));
}

static uint8_t
tpm_tis_locality(uint64_t off)
{
	return (off / TPM_TIS_LOCALITY_SIZE);
}

static uint16_t
tpm_tis_reg(uint64_t off)
{
	uint16_t reg;

	reg = off & (TPM_TIS_LOCALITY_SIZE - 1);

	switch (reg) {
	case TPM_ACCESS:
		return (TPM_ACCESS);
	case TPM_INT_ENABLE ... TPM_INT_ENABLE + 3:
		return (TPM_INT_ENABLE);
	case TPM_INT_VECTOR:
		return (TPM_INT_VECTOR);
	case TPM_INT_STATUS ... TPM_INT_STATUS + 3:
		return (TPM_INT_STATUS);
	case TPM_INTF_CAPABILITY ... TPM_INTF_CAPABILITY + 3:
		return (TPM_INTF_CAPABILITY);
	case TPM_STS ... TPM_STS + 3:
		return (TPM_STS);
	case TPM_DATA_FIFO ... TPM_DATA_FIFO + 3:
	case TPM_XDATA_FIFO ... TPM_XDATA_FIFO_END:
		return (TPM_DATA_FIFO);
	case TPM_INTERFACE_ID ... TPM_INTERFACE_ID + 3:
		return (TPM_INTERFACE_ID);
	case TPM_DID_VID ... TPM_DID_VID + 3:
		return (TPM_DID_VID);
	case TPM_RID:
		return (TPM_RID);
	default:
		return (reg & ~0x3);
	}
}

static uint32_t
tpm_tis_mask(int size)
{
	switch (size) {
	case 1:
		return (0xff);
	case 2:
		return (0xffff);
	case 4:
		return (0xffffffffu);
	default:
		return (0);
	}
}

static bool
tpm_tis_has_single_bit(uint32_t val)
{
	return (val != 0 && (val & (val - 1)) == 0);
}

static void
tpm_tis_set_sts(struct tpm_tis_locality *loc, uint32_t sts)
{
	loc->sts = TPM_STS_TPM20 | sts;
}

static void
tpm_tis_reset_locality(struct tpm_tis_locality *loc)
{
	loc->state = TPM_STATE_IDLE;
	loc->access = TPM_ACCESS_TPM_REG_VALID_STS;
	loc->sts = TPM_STS_TPM20;
	loc->iface_id = TPM_INTF_ID_SUPPORTED_FLAGS;
	loc->int_enable = TPM_INT_POLARITY_LOW_LEVEL;
	loc->int_status = 0;
}

static void
tpm_tis_set_active_locality(struct tpm_tis *tis, uint8_t locality)
{
	uint8_t i;

	if (tis->active_locality < TPM_TIS_NUM_LOCALITIES)
		tis->loc[tis->active_locality].access &=
		    ~TPM_ACCESS_ACTIVE_LOCALITY;

	tis->active_locality = locality;
	if (locality >= TPM_TIS_NUM_LOCALITIES)
		return;

	for (i = 0; i < TPM_TIS_NUM_LOCALITIES; i++)
		tis->loc[i].access &= ~TPM_ACCESS_PENDING_REQUEST;

	tis->loc[locality].access &= ~TPM_ACCESS_REQUEST_USE;
	tis->loc[locality].access |= TPM_ACCESS_ACTIVE_LOCALITY;
}

static uint32_t
tpm_tis_sts_read(struct tpm_tis *tis, uint8_t locality, int size)
{
	struct tpm_tis_locality *loc;
	uint32_t avail, val;

	loc = &tis->loc[locality];
	if (tis->active_locality != locality)
		return (0xffffffffu);

	if (loc->sts & TPM_STS_DATA_AVAIL) {
		avail = tpm_tis_cmd_size(tis->buffer);
		if (avail > TPM_TIS_BUFFER_SIZE)
			avail = TPM_TIS_BUFFER_SIZE;
		if (avail > tis->rw_offset)
			avail -= tis->rw_offset;
		else
			avail = 0;
	} else {
		avail = TPM_TIS_BUFFER_SIZE - tis->rw_offset;
		if (size == 1 && avail > 0xff)
			avail = 0xff;
	}

	val = loc->sts | TPM_STS_BURST_COUNT(avail);
	return (val);
}

static uint8_t
tpm_tis_fifo_read(struct tpm_tis *tis, uint8_t locality)
{
	struct tpm_tis_locality *loc;
	uint32_t len;

	loc = &tis->loc[locality];
	if (tis->active_locality != locality ||
	    loc->state != TPM_STATE_COMPLETION)
		return (TPM_NO_DATA_BYTE);

	len = tpm_tis_cmd_size(tis->buffer);
	if (len > TPM_TIS_BUFFER_SIZE)
		len = TPM_TIS_BUFFER_SIZE;
	if (tis->rw_offset >= len) {
		loc->sts &= ~TPM_STS_DATA_AVAIL;
		return (TPM_NO_DATA_BYTE);
	}

	if (tis->rw_offset + 1 >= len)
		loc->sts &= ~TPM_STS_DATA_AVAIL;

	return (tis->buffer[tis->rw_offset++]);
}

static void
tpm_tis_fifo_write(struct tpm_tis *tis, uint8_t locality, uint8_t value)
{
	struct tpm_tis_locality *loc;
	uint32_t len;

	loc = &tis->loc[locality];
	if (tis->active_locality != locality ||
	    loc->state == TPM_STATE_IDLE ||
	    loc->state == TPM_STATE_EXECUTION ||
	    loc->state == TPM_STATE_COMPLETION)
		return;

	if (loc->state == TPM_STATE_READY) {
		loc->state = TPM_STATE_RECEPTION;
		tpm_tis_set_sts(loc, TPM_STS_DATA_EXPECTED | TPM_STS_VALID);
	}

	if ((loc->sts & TPM_STS_DATA_EXPECTED) == 0)
		return;

	if (tis->rw_offset < TPM_TIS_BUFFER_SIZE)
		tis->buffer[tis->rw_offset++] = value;

	if (tis->rw_offset >
	    offsetof(struct tpm_cmd_hdr, len) + sizeof(uint32_t)) {
		len = tpm_tis_cmd_size(tis->buffer);
		if (len < sizeof(struct tpm_cmd_hdr) ||
		    len > TPM_TIS_BUFFER_SIZE || len <= tis->rw_offset)
			tpm_tis_set_sts(loc, TPM_STS_VALID);
		else
			tpm_tis_set_sts(loc,
			    TPM_STS_DATA_EXPECTED | TPM_STS_VALID);
	}
}

static void
tpm_tis_command_ready(struct tpm_tis *tis, uint8_t locality)
{
	struct tpm_tis_locality *loc;

	loc = &tis->loc[locality];
	memset(tis->buffer, 0, sizeof(tis->buffer));
	tis->rw_offset = 0;
	loc->state = TPM_STATE_READY;
	tpm_tis_set_sts(loc, TPM_STS_CMD_RDY | TPM_STS_VALID);
}

static void
tpm_tis_execute(struct tpm_tis *tis, uint8_t locality)
{
	struct tpm_tis_locality *loc;
	uint8_t rsp[TPM_TIS_BUFFER_SIZE];
	uint32_t len;
	int error;

	loc = &tis->loc[locality];
	if (loc->state != TPM_STATE_RECEPTION ||
	    (loc->sts & TPM_STS_DATA_EXPECTED) != 0)
		return;

	len = tpm_tis_cmd_size(tis->buffer);
	if (len < sizeof(struct tpm_cmd_hdr) || len > TPM_TIS_BUFFER_SIZE) {
		warnx("%s: invalid TPM request length %u", __func__, len);
		tpm_tis_command_ready(tis, locality);
		return;
	}

	loc->state = TPM_STATE_EXECUTION;
	tpm_tis_set_sts(loc, TPM_STS_VALID);
	memset(rsp, 0, sizeof(rsp));

	error = tis->emul->execute_cmd(tis->emul_sc, tis->buffer, len, rsp,
	    sizeof(rsp));

	if (error != 0) {
		warnx("%s: cmd 0x%08x len %u failed: %d", __func__,
		    be32toh(((struct tpm_cmd_hdr *)tis->buffer)->ordinal), len,
		    error);
		tpm_tis_command_ready(tis, locality);
		return;
	}

	memcpy(tis->buffer, rsp, sizeof(tis->buffer));
	tis->rw_offset = 0;
	loc->state = TPM_STATE_COMPLETION;
	tpm_tis_set_sts(loc, TPM_STS_DATA_AVAIL | TPM_STS_VALID);
}

static uint32_t
tpm_tis_reg_read(struct tpm_tis *tis, uint8_t locality, uint16_t reg, int size,
    uint64_t addr)
{
	struct tpm_tis_locality *loc;
	uint32_t val;
	uint8_t shift;
	int read_size;

	loc = &tis->loc[locality];
	shift = (addr & 0x3) * 8;
	read_size = size;
	val = 0xffffffffu;

	switch (reg) {
	case TPM_ACCESS:
		val = loc->access & ~TPM_ACCESS_SEIZE;
		val |= TPM_ACCESS_TPM_ESTABLISHMENT;
		break;
	case TPM_INT_ENABLE:
		val = loc->int_enable;
		break;
	case TPM_INT_VECTOR:
		val = 0;
		break;
	case TPM_INT_STATUS:
		val = loc->int_status;
		break;
	case TPM_INTF_CAPABILITY:
		val = TPM_INTF_CAPS_SUPPORTED;
		break;
	case TPM_STS:
		val = tpm_tis_sts_read(tis, locality, size);
		break;
	case TPM_DATA_FIFO:
		if (size > 4 - (addr & 0x3))
			size = 4 - (addr & 0x3);
		read_size = size;
		val = 0;
		shift = 0;
		while (size-- > 0) {
			val |= (uint32_t)tpm_tis_fifo_read(tis, locality)
			    << shift;
			shift += 8;
		}
		shift = 0;
		break;
	case TPM_INTERFACE_ID:
		val = loc->iface_id;
		break;
	case TPM_DID_VID:
		val = (TPM_DID_VALUE << 16) | TPM_VID_VALUE;
		break;
	case TPM_RID:
		val = TPM_RID_VALUE;
		break;
	default:
		break;
	}

	if (shift != 0)
		val >>= shift;

	return (val & tpm_tis_mask(read_size));
}

static void
tpm_tis_reg_write(struct tpm_tis *tis, uint8_t locality, uint16_t reg, int size,
    uint64_t addr, uint32_t val)
{
	struct tpm_tis_locality *loc;
	uint32_t mask;
	uint8_t shift;

	loc = &tis->loc[locality];
	mask = tpm_tis_mask(size);
	shift = (addr & 0x3) * 8;
	val &= mask;
	if (shift != 0) {
		val <<= shift;
		mask <<= shift;
	}

	switch (reg) {
	case TPM_ACCESS:
		if ((val & TPM_ACCESS_SEIZE) != 0)
			val &= TPM_ACCESS_SEIZE;
		else if (!tpm_tis_has_single_bit(val))
			break;

		if (val & TPM_ACCESS_ACTIVE_LOCALITY) {
			if (tis->active_locality == locality)
				tpm_tis_set_active_locality(tis,
				    TPM_NO_LOCALITY);
			else
				loc->access &= ~TPM_ACCESS_REQUEST_USE;
		}
		if (val & TPM_ACCESS_BEEN_SEIZED)
			loc->access &= ~TPM_ACCESS_BEEN_SEIZED;
		if (val & TPM_ACCESS_REQUEST_USE) {
			if (tis->active_locality == TPM_NO_LOCALITY)
				tpm_tis_set_active_locality(tis, locality);
			else if (tis->active_locality != locality)
				loc->access |= TPM_ACCESS_REQUEST_USE;
		}
		if (val & TPM_ACCESS_SEIZE)
			tpm_tis_set_active_locality(tis, locality);
		break;
	case TPM_INT_ENABLE:
		loc->int_enable &= ~mask;
		loc->int_enable |= val &
		    (TPM_INT_ENABLED | TPM_INT_POLARITY_MASK |
			TPM_INT_SUPPORTED);
		break;
	case TPM_INT_STATUS:
		loc->int_status &= ~(val & TPM_INT_SUPPORTED);
		break;
	case TPM_STS:
		if (tis->active_locality != locality)
			break;
		val &= TPM_STS_CMD_RDY | TPM_STS_CMD_START |
		    TPM_STS_RESP_RETRY | TPM_STS_CMD_CANCEL;
		if (val == TPM_STS_CMD_RDY)
			tpm_tis_command_ready(tis, locality);
		else if (val == TPM_STS_CMD_START)
			tpm_tis_execute(tis, locality);
		else if (val == TPM_STS_RESP_RETRY &&
		    loc->state == TPM_STATE_COMPLETION) {
			tis->rw_offset = 0;
			tpm_tis_set_sts(loc,
			    TPM_STS_DATA_AVAIL | TPM_STS_VALID);
		}
		break;
	case TPM_DATA_FIFO:
		if (size > 4 - (addr & 0x3))
			size = 4 - (addr & 0x3);
		val >>= shift;
		while (size-- > 0) {
			tpm_tis_fifo_write(tis, locality, val & 0xff);
			val >>= 8;
		}
		break;
	case TPM_INTERFACE_ID:
		if (val & TPM_INTF_ID_INT_SEL_LOCK) {
			for (uint8_t i = 0; i < TPM_TIS_NUM_LOCALITIES; i++)
				tis->loc[i].iface_id |=
				    TPM_INTF_ID_INT_SEL_LOCK;
		}
		break;
	default:
		break;
	}
}

static int
tpm_tis_mem_handler(struct vcpu *vcpu __unused, int dir, uint64_t addr,
    int size, uint64_t *val, void *arg1, long arg2 __unused)
{
	struct tpm_tis *tis;
	uint64_t off;
	uint16_t reg;
	uint8_t locality;

	if (!(size == 1 || size == 2 || size == 4))
		return (EINVAL);

	tis = arg1;
	off = addr - TPM_TIS_ADDRESS;
	if (off >= TPM_TIS_MMIO_SIZE || off + size > TPM_TIS_MMIO_SIZE)
		return (EINVAL);

	locality = tpm_tis_locality(off);
	if (locality >= TPM_TIS_NUM_LOCALITIES)
		return (EINVAL);

	reg = tpm_tis_reg(off);

	pthread_mutex_lock(&tis->mutex);
	if (dir == MEM_F_READ)
		*val = tpm_tis_reg_read(tis, locality, reg, size, addr);
	else
		tpm_tis_reg_write(tis, locality, reg, size, addr, *val);

	pthread_mutex_unlock(&tis->mutex);

	return (0);
}

static int
tpm_tis_modify_mmio_registration(bool registration, void *arg1)
{
	struct mem_range tis_mmio = {
		.name = "tpm-tis-mmio",
		.base = TPM_TIS_ADDRESS,
		.size = TPM_TIS_MMIO_SIZE,
		.flags = MEM_F_RW,
		.arg1 = arg1,
		.handler = tpm_tis_mem_handler,
	};

	if (registration)
		return (register_mem(&tis_mmio));

	return (unregister_mem(&tis_mmio));
}

static int
tpm_tis_init(void **sc, struct tpm_emul *emul, void *emul_sc,
    struct acpi_device *acpi_dev)
{
	struct tpm_tis *tis;
	int error;

	assert(sc != NULL);
	assert(emul != NULL);

	tis = calloc(1, sizeof(*tis));
	if (tis == NULL)
		return (ENOMEM);

	tis->emul = emul;
	tis->emul_sc = emul_sc;
	tis->active_locality = TPM_NO_LOCALITY;
	for (uint8_t i = 0; i < TPM_TIS_NUM_LOCALITIES; i++)
		tpm_tis_reset_locality(&tis->loc[i]);

	error = acpi_device_add_res_fixed_memory32(acpi_dev, false,
	    TPM_TIS_ADDRESS, TPM_TIS_MMIO_SIZE);
	if (error)
		goto err_out;

	error = pthread_mutex_init(&tis->mutex, NULL);
	if (error) {
		warnc(error, "%s: failed to init mutex", __func__);
		goto err_out;
	}
	tis->mutex_initialized = true;

	error = tpm_tis_modify_mmio_registration(true, tis);
	if (error) {
		warnc(error, "%s: failed to register TIS MMIO", __func__);
		goto err_out;
	}
	tis->mmio_registered = true;

	*sc = tis;

	return (0);

err_out:
	if (tis->mmio_registered)
		(void)tpm_tis_modify_mmio_registration(false, NULL);
	if (tis->mutex_initialized)
		pthread_mutex_destroy(&tis->mutex);
	free(tis);

	return (error);
}

static void
tpm_tis_deinit(void *sc)
{
	struct tpm_tis *tis;

	tis = sc;
	if (tis == NULL)
		return;

	if (tis->mmio_registered)
		(void)tpm_tis_modify_mmio_registration(false, NULL);
	pthread_mutex_destroy(&tis->mutex);
	free(tis);
}

static struct tpm_intf tpm_intf_tis = {
	.name = TPM_INTF_TIS_NAME,
	.init = tpm_tis_init,
	.deinit = tpm_tis_deinit,
};
TPM_INTF_SET(tpm_intf_tis);
