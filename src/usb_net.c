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

#include <sys/cdefs.h>
#include <sys/time.h>
#include "common.h"

// #include <machine/vmm_snapshot.h>
#include <sys/param.h>
#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv.h"

#include "usb.h"
#include "usb_cdc.h"
#include "usbdi.h"

#include "bhyvegc.h"
#include "config.h"
#include "console.h"
#include "debug.h"
#include "iov.h"
#include "net_backends.h"
#include "net_backends_priv.h"
#include "usb_emul.h"

static int unet_debug = 0;
#define DPRINTF(params) \
	if (unet_debug) \
	PRINTLN params
#define WPRINTF(params) PRINTLN params

#define MSETW(ptr, val) ptr = { (uint8_t)(val), (uint8_t)((val) >> 8) }
#define MSETDW(ptr, val)													\
	ptr = { (uint8_t)(val), (uint8_t)((val) >> 8), (uint8_t)((val) >> 16),	\
		(uint8_t)((val) >> 24) }
#define MSETQW(ptr, val1, val2)                                               	\
	ptr = { (uint8_t)(val1), (uint8_t)((val1) >> 8), (uint8_t)((val1) >> 16),	\
		(uint8_t)((val1) >> 24), (uint8_t)(val2), (uint8_t)((val2) >> 8),		\
		(uint8_t)((val2) >> 16), (uint8_t)((val2) >> 24)}

#define DEFAULT_MAC_ADDR "00CD563B4270"
#define UNET_MTU_SIZE	 1514
#define UNET_NTB_MAX_SIZE 65536
#define UNET_NTB_MAX_FRAMES 32

enum {
	UMSTR_LANG,
	UMSTR_MANUFACTURER,
	UMSTR_PRODUCT,
	UMSTR_SERIAL,
	UMSTR_CONFIG,
	UMSTR_MAC,
	UMSTR_DATA_ALT0,
	UMSTR_DATA_ALT1,
	UMSTR_MAX
};

static const char *unet_desc_strings[] = {
	"\x09\x04",
	"SCORPI",
	"Scorpi USB Network Card",
	"01",
	"Ethernet Device",
	DEFAULT_MAC_ADDR,
	"CDC_DATA0",
	"CDC_DATA1",
};

#define UNET_NOTIFY_ENDPT 1

struct usb_ss_ep_comp_descriptor {
	uByte bLength;		 // 6 bytes
	uByte bDescriptorType;	 // 0x30 (SuperSpeed Endpoint Companion)
	uByte bMaxBurst;	 // Maximum burst (1-15 for bulk endpoints)
	uByte bmAttributes;	 // 0 for bulk endpoints
	uWord wBytesPerInterval; // Only used for isochronous endpoints
} __attribute__((packed));

struct usb_cdc_ncm_config_desc {
	struct usb_config_descriptor confd;
	struct usb_interface_assoc_descriptor cdc_iad;
	struct usb_interface_descriptor comm_ifcd_alt0;
	struct usb_cdc_header_descriptor cdc_header;
	struct usb_cdc_union_descriptor cdc_union;
	struct usb_cdc_ethernet_descriptor eth_desc;
	struct usb_ncm_func_descriptor ncm_func_desc;
	struct usb_endpoint_descriptor notify_ep;
	struct usb_ss_ep_comp_descriptor notify_ss_comp;
	struct usb_interface_descriptor data_ifcd_alt0;
	struct usb_interface_descriptor data_ifcd_alt1;
	struct usb_endpoint_descriptor bulk_out_ep;
	struct usb_ss_ep_comp_descriptor bulk_out_ss_comp;
	struct usb_endpoint_descriptor bulk_in_ep;
	struct usb_ss_ep_comp_descriptor bulk_in_ss_comp;
} __packed;

struct usb_cdc_ncm_config_desc unet_confd = {
    .confd = {
        .bLength = sizeof(unet_confd.confd),
        .bDescriptorType = UDESC_CONFIG,
        MSETW(.wTotalLength, sizeof(unet_confd)),
        .bNumInterface = 2,
        .bConfigurationValue = 1,
        .iConfiguration = UMSTR_CONFIG,
        .bmAttributes = UC_BUS_POWERED,
        .bMaxPower = 0x32, // 100 mA
    },
    .cdc_iad = {
        .bLength = sizeof(unet_confd.cdc_iad),
        .bDescriptorType = UDESC_IFACE_ASSOC,
        .bFirstInterface = 0,       // Communication interface
        .bInterfaceCount = 2,       // Data interface
        .bFunctionClass = UICLASS_CDC,
        .bFunctionSubClass = UISUBCLASS_NETWORK_CONTROL_MODEL,
        .bFunctionProtocol = 0,
        .iFunction = UMSTR_CONFIG,
    },
     .comm_ifcd_alt0 = {
        .bLength = sizeof(unet_confd.comm_ifcd_alt0),
        .bDescriptorType = UDESC_INTERFACE,
        .bInterfaceNumber = 0,
        .bAlternateSetting = 0,
        .bNumEndpoints = 1,
        .bInterfaceClass = UICLASS_CDC,
        .bInterfaceSubClass = UISUBCLASS_NETWORK_CONTROL_MODEL,
        .bInterfaceProtocol = 0,
        .iInterface = 0,
    },
    .cdc_header = {
        .bLength = sizeof(unet_confd.cdc_header),
        .bDescriptorType = UDESC_CS_INTERFACE,
        .bDescriptorSubtype = 0,
        MSETW(.bcdCDC, 0x0110),
    },
    .cdc_union = {
        .bLength = sizeof(unet_confd.cdc_union),
        .bDescriptorType = UDESC_CS_INTERFACE,
        .bDescriptorSubtype = UDESCSUB_CDC_UNION,
        .bMasterInterface = 0,      // Communication interface
        .bSlaveInterface = {1},       // Data interface
    },
    .eth_desc = {
        .bLength = sizeof(unet_confd.eth_desc),
        .bDescriptorType = UDESC_CS_INTERFACE,
        .bDescriptorSubtype = 0x0F,
        .iMacAddress = UMSTR_MAC,
        .bmEthernetStatistics = {0},
        MSETW(.wMaxSegmentSize, UNET_MTU_SIZE),
        .wNumberMCFilters = {0},
        .bNumberPowerFilters = 0,
    },
    .ncm_func_desc = {
        .bLength = sizeof(unet_confd.ncm_func_desc),
        .bDescriptorType = UDESC_CS_INTERFACE,
        .bDescriptorSubtype = UCDC_NCM_FUNC_DESC_SUBTYPE,
        MSETW(.bcdNcmVersion, 0x0100),
        .bmNetworkCapabilities = UCDC_NCM_CAP_FILTER | UCDC_NCM_CAP_MAC_ADDR | 
            UCDC_NCM_CAP_ENCAP | UCDC_NCM_CAP_MAX_DATA | UCDC_NCM_CAP_MAX_DGRAM,
    },
    .notify_ep = {
        .bLength = sizeof(unet_confd.notify_ep),
        .bDescriptorType = UDESC_ENDPOINT,
        .bEndpointAddress = UE_DIR_IN | UNET_NOTIFY_ENDPT, // endpoint 1
        .bmAttributes = UE_INTERRUPT,
        .wMaxPacketSize[0] = 16,
        .bInterval = 0xa,      // polling interval 10 ms
    },
    .notify_ss_comp = {
        .bLength = sizeof(unet_confd.notify_ss_comp),
        .bDescriptorType = UDESC_ENDPOINT_SS_COMP,
        .bMaxBurst = 0,
        .bmAttributes = 0,
    },
    .data_ifcd_alt0 = {
        .bLength = sizeof(struct usb_interface_descriptor),
        .bDescriptorType = UDESC_INTERFACE,
        .bInterfaceNumber = 1,
        .bAlternateSetting = 0,
        .bNumEndpoints = 0,
        .bInterfaceClass = UICLASS_CDC_DATA,
        .bInterfaceSubClass = 0,
        .bInterfaceProtocol = 0,
        .iInterface = UMSTR_DATA_ALT0,
    },
    .data_ifcd_alt1 = {
        .bLength = sizeof(struct usb_interface_descriptor),
        .bDescriptorType = UDESC_INTERFACE,
        .bInterfaceNumber = 1,
        .bAlternateSetting = 1,
        .bNumEndpoints = 2,  //  Bulk IN + Bulk OUT
        .bInterfaceClass = UICLASS_CDC_DATA,
        .bInterfaceSubClass = 0,
        .bInterfaceProtocol = UIPROTO_DATA_NCM,
        .iInterface = UMSTR_DATA_ALT1,
    },
    .bulk_out_ep = {
        .bLength = sizeof(unet_confd.bulk_out_ep),
        .bDescriptorType = UDESC_ENDPOINT,
        .bEndpointAddress = UE_DIR_OUT | 2, // endpoint 2
        .bmAttributes = UE_BULK,
        MSETW(.wMaxPacketSize, 1024),
        .bInterval = 0,
    },
     .bulk_out_ss_comp = {
        .bLength = sizeof(unet_confd.bulk_out_ss_comp),
        .bDescriptorType = UDESC_ENDPOINT_SS_COMP,
        .bMaxBurst = 4,
        .bmAttributes = 0,
    },
    .bulk_in_ep = {
        .bLength = sizeof(unet_confd.bulk_in_ep),
        .bDescriptorType = UDESC_ENDPOINT,
        .bEndpointAddress = UE_DIR_IN | 3, // endpoint 3
        .bmAttributes = UE_BULK,
        MSETW(.wMaxPacketSize, 1024),
        .bInterval = 0,
    },
    .bulk_in_ss_comp = {
        .bLength = sizeof(unet_confd.bulk_in_ss_comp),
        .bDescriptorType = UDESC_ENDPOINT_SS_COMP,
        .bMaxBurst = 4,
        .bmAttributes = 0,
    },
};

// CDC-NCM Device Descriptor Initialization
usb_device_descriptor_t unet_dev_desc = { .bLength = sizeof(
					      usb_device_descriptor_t),
	.bDescriptorType = UDESC_DEVICE,
	MSETW(.bcdUSB, UD_USB_3_0),
	.bDeviceClass = 0x02, // CDC (Communications Device Class)
	.bDeviceSubClass = 0x00,
	.bDeviceProtocol = 0x00,
	.bMaxPacketSize = 9,
	MSETW(.idVendor, 0xFB5D),
	MSETW(.idProduct, 0x0003),
	MSETW(.bcdDevice, 0),
	.iManufacturer = UMSTR_MANUFACTURER,
	.iProduct = UMSTR_PRODUCT,
	.iSerialNumber = UMSTR_SERIAL,
	.bNumConfigurations = 1 };

struct unet_bos_desc {
	struct usb_bos_descriptor bosd;
	struct usb_devcap_ss_descriptor usbssd;
} __packed;

static struct unet_bos_desc unet_bosd = {
	.bosd = {
		.bLength = sizeof(unet_bosd.bosd),
		.bDescriptorType = UDESC_BOS,
		MSETW(.wTotalLength, sizeof(unet_bosd)),
		.bNumDeviceCaps = 1,
	},
	.usbssd = {
		.bLength = sizeof(unet_bosd.usbssd),
		.bDescriptorType = UDESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = 3,
		.bmAttributes = 2,
		MSETW(.wSpeedsSupported, 0x0e),
		.bFunctionalitySupport = USB_DEVCAP_SUPER_SPEED,
		.bU1DevExitLat = 0xa,   /* dummy - not used */
		.wU2DevExitLat = { 0x20, 0x00 },
	}
};

struct usb_ncm_parameters ncm_params = {
	MSETW(.wLength, sizeof(struct usb_ncm_parameters)),
	.bmNtbFormatsSupported[0] = UCDC_NCM_FORMAT_NTB16,
	MSETDW(.dwNtbInMaxSize, 8192),
	.wNdpInDivisor[0] = 4,
	.wNdpInPayloadRemainder[0] = 0,
	.wNdpInAlignment[0] = 4,
	MSETDW(.dwNtbOutMaxSize, 8192),
	.wNdpOutDivisor[0] = 4,
	.wNdpOutPayloadRemainder = { 0 },
	.wNdpOutAlignment[0] = 4,
	.wNtbOutMaxDatagrams[0] = 16,
};

enum unet_notify_state {
	UNET_STATE_NOTIFY_LINK_SPEED,
	UNET_STATE_NOTIFY_LINK_UP,
	UNET_STATE_NOTIFY_IDLE,
};

struct unet_softc {
	struct usb_hci *hci;

	uint8_t mac_addr[6];

	uint16_t ntb_format;
	uint16_t max_dgram;
	uint32_t ntb_input_size;
	enum unet_notify_state notify_state;

	uint16_t seq;

	pthread_mutex_t mtx;
	pthread_mutex_t ev_mtx;

	net_backend_t *vsc_be;
};

static void
unet_rx_callback(int fd __unused, enum ev_type type __unused, void *param)
{
	struct unet_softc *sc = param;

	DPRINTF(("unet_rx_callback"));
	sc->hci->hci_intr(sc->hci, UE_DIR_IN | 3);
}

static void
unet_transmit_backend(struct unet_softc *sc, struct iovec *iov, int iovcnt)
{
	if (sc->vsc_be == NULL)
		return;

	netbe_send(sc->vsc_be, iov, iovcnt);
}

static bool
unet_ntb_format_supported(uint16_t format)
{
	return (format == UCDC_NCM_FORMAT_NTB16);
}

static bool
is_valid_mac_string(const char *mac_str)
{
	int i;
	if (strlen(mac_str) != 12) {
		return (false);
	}
	for (i = 0; i < 12; i++) {
		if (!isxdigit(mac_str[i])) {
			return (false);
		}
	}
	return (true);
}

static int
parse_mac_address(const char *mac_str, uint8_t *mac)
{
	int i;
	if (!is_valid_mac_string(mac_str)) {
		return (-1);
	}

	for (i = 0; i < 6; i++) {
		char hex_byte[3] = { mac_str[i * 2], mac_str[i * 2 + 1], '\0' };
		mac[i] = (uint8_t)strtol(hex_byte, NULL, 16);
	}
	return (0);
}

static void *
unet_init(struct usb_hci *hci, nvlist_t *nvl)
{
	struct unet_softc *sc;
	int err;
	pthread_mutexattr_t attr;
	const char *mac;

	sc = calloc(1, sizeof(struct unet_softc));
	sc->hci = hci;

	sc->ntb_format = UCDC_NCM_FORMAT_NTB16;
	sc->max_dgram = 8192;
	sc->ntb_input_size = UNET_NTB_MAX_SIZE;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sc->mtx, &attr);
	pthread_mutex_init(&sc->ev_mtx, &attr);

	/* Permit interfaces without a configured backend. */
	if (get_config_value_node(nvl, "backend") != NULL) {
		err = netbe_init(&sc->vsc_be, nvl, unet_rx_callback, sc);
		if (err) {
			free(sc);
			return (NULL);
		}
	}

	mac = get_config_value_node(nvl, "mac");
	if (mac != NULL) {
		if (parse_mac_address(mac, sc->mac_addr) != 0) {
			EPRINTLN("invalid mac address");
			free(sc);
			return (NULL);
		}
		unet_desc_strings[UMSTR_MAC] = mac;
	} else {
		parse_mac_address(DEFAULT_MAC_ADDR, sc->mac_addr);
	}

	return (sc);
}

#define UREQ(x, y) ((x) | ((y) << 8))

static inline uint16_t
usb_extract_u16(const void *data)
{
	const uint8_t *bytes = (const uint8_t *)data;
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static inline uint32_t
usb_extract_u32(const void *data)
{
	const uint8_t *bytes = (const uint8_t *)data;
	return (uint32_t)(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
	    (bytes[3] << 24));
}

static int
unet_request(void *scarg, struct usb_data_xfer *xfer)
{
	struct unet_softc *sc;
	struct usb_data_xfer_block *data;
	const char *str;
	uint16_t value;
	uint16_t index;
	uint16_t len;
	uint16_t slen;
	uint8_t *udata;
	int err;
	int i, idx;
	int eshort;

	sc = scarg;
	pthread_mutex_lock(&sc->mtx);

	data = NULL;
	udata = NULL;
	idx = xfer->head;
	for (i = 0; i < xfer->ndata; i++) {
		xfer->data[idx].bdone = 0;
		if (data == NULL && USB_DATA_OK(xfer, idx)) {
			data = &xfer->data[idx];
			udata = data->buf;
		}

		xfer->data[idx].processed = 1;
		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
	}

	err = USB_ERR_NORMAL_COMPLETION;
	eshort = 0;

	if (!xfer->ureq) {
		DPRINTF(("net_request: port %d", sc->hci->hci_port));
		goto done;
	}

	value = UGETW(xfer->ureq->wValue);
	index = UGETW(xfer->ureq->wIndex);
	len = UGETW(xfer->ureq->wLength);

	DPRINTF(("unet_request: port %d, type 0x%x, req 0x%x, val 0x%x, "
		 "idx 0x%x, len %u",
	    sc->hci->hci_port, xfer->ureq->bmRequestType, xfer->ureq->bRequest,
	    value, index, len));

	switch (UREQ(xfer->ureq->bRequest, xfer->ureq->bmRequestType)) {
	case UREQ(UR_GET_CONFIG, UT_READ_DEVICE):
		DPRINTF(("unet: (UR_GET_CONFIG, UT_READ_DEVICE)"));
		if (!data)
			break;

		*udata = unet_confd.confd.bConfigurationValue;
		data->blen = len > 0 ? len - 1 : 0;
		eshort = data->blen > 0;
		data->bdone += 1;
		break;

	case UREQ(UR_GET_DESCRIPTOR, UT_READ_DEVICE):
		DPRINTF((
		    "unet: (UR_GET_DESCRIPTOR, UT_READ_DEVICE) val %x, data %p",
		    value >> 8, data));
		if (!data)
			break;

		switch (value >> 8) {
		case UDESC_DEVICE:
			DPRINTF(("unet: (->UDESC_DEVICE) len %u ?= "
				 "sizeof(unet_dev_desc) %lu",
			    len, sizeof(unet_dev_desc)));
			if ((value & 0xFF) != 0) {
				err = USB_ERR_STALLED;
				goto done;
			}
			if (len > sizeof(unet_dev_desc)) {
				data->blen = len - sizeof(unet_dev_desc);
				len = sizeof(unet_dev_desc);
			} else
				data->blen = 0;
			memcpy(data->buf, &unet_dev_desc, len);
			data->bdone += len;
			break;

		case UDESC_CONFIG:
			DPRINTF((
			    "unet: (->UDESC_CONFIG) read len %lu, passed len %d",
			    sizeof(unet_confd), len));
			if ((value & 0xFF) != 0) {
				err = USB_ERR_STALLED;
				goto done;
			}
			if (len > sizeof(unet_confd)) {
				data->blen = len - sizeof(unet_confd);
				len = sizeof(unet_confd);
			} else
				data->blen = 0;

			memcpy(data->buf, &unet_confd, len);
			data->bdone += len;
			break;

		case UDESC_STRING:
			DPRINTF(("unet: (->UDESC_STRING %x)", value & 0xFF));
			str = NULL;
			if ((value & 0xFF) < UMSTR_MAX)
				str = unet_desc_strings[value & 0xFF];
			else
				goto done;

			if ((value & 0xFF) == UMSTR_LANG) {
				udata[0] = 4;
				udata[1] = UDESC_STRING;
				data->blen = len - 2;
				len -= 2;
				data->bdone += 2;

				if (len >= 2) {
					udata[2] = str[0];
					udata[3] = str[1];
					data->blen -= 2;
					data->bdone += 2;
				} else
					data->blen = 0;

				goto done;
			}

			slen = 2 + strlen(str) * 2;
			udata[0] = slen;
			udata[1] = UDESC_STRING;

			if (len > slen) {
				data->blen = len - slen;
				len = slen;
			} else
				data->blen = 0;
			for (i = 2; i < len; i += 2) {
				udata[i] = *str++;
				udata[i + 1] = '\0';
			}
			data->bdone += len;

			break;

		case UDESC_BOS:
			DPRINTF(("unet: USB3 BOS"));
			if (len > sizeof(unet_bosd)) {
				data->blen = len - sizeof(unet_bosd);
				len = sizeof(unet_bosd);
			} else
				data->blen = 0;
			memcpy(udata, &unet_bosd, len);
			data->bdone += len;
			break;

		default:
			DPRINTF(("unet: unknown(%d)->ERROR", value >> 8));
			err = USB_ERR_STALLED;
			goto done;
		}
		if (data)
			eshort = data->blen > 0;
		break;

	case UREQ(UR_GET_DESCRIPTOR, UT_READ_INTERFACE):
		DPRINTF(("unet: (UR_GET_DESCRIPTOR, UT_READ_INTERFACE) "
			 "0x%x",
		    (value >> 8)));
		if (!data)
			break;
		break;

	case UREQ(UR_GET_INTERFACE, UT_READ_INTERFACE):
		DPRINTF(("unet: (UR_GET_INTERFACE, UT_READ_INTERFACE)"));
		if (index != 0) {
			DPRINTF(
			    ("unet get_interface, invalid index %d", index));
			err = USB_ERR_STALLED;
			goto done;
		}

		if (!data)
			break;

		if (len > 0) {
			*udata = 0;
			data->blen = len - 1;
		}
		eshort = data->blen > 0;
		data->bdone += 1;
		break;

	case UREQ(UR_GET_STATUS, UT_READ_DEVICE):
		DPRINTF(("unet: (UR_GET_STATUS, UT_READ_DEVICE)"));
		if (data != NULL && len > 1) {
			USETW(udata, 0);
			data->blen = len - 2;
			data->bdone += 2;
		}
		if (data)
			eshort = data->blen > 0;
		break;

	case UREQ(UR_GET_STATUS, UT_READ_INTERFACE):
	case UREQ(UR_GET_STATUS, UT_READ_ENDPOINT):
		DPRINTF(("unet: (UR_GET_STATUS, UT_READ_INTERFACE)"));
		if (data != NULL && len > 1) {
			USETW(udata, 0);
			data->blen = len - 2;
			data->bdone += 2;
		}
		if (data)
			eshort = data->blen > 0;
		break;

	case UREQ(UR_SET_ADDRESS, UT_WRITE_DEVICE):
		/* XXX Controller should've handled this */
		DPRINTF(("unet set address %u", value));
		break;

	case UREQ(UR_SET_CONFIG, UT_WRITE_DEVICE):
		DPRINTF(("unet set config %u", value));
		sc->notify_state = UNET_STATE_NOTIFY_LINK_SPEED;
		break;

	case UREQ(UR_SET_DESCRIPTOR, UT_WRITE_DEVICE):
		DPRINTF(("unet set descriptor %u", value));
		break;

	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_DEVICE):
		DPRINTF(("unet: (UR_SET_FEATURE, UT_WRITE_DEVICE) %x", value));
		break;

	case UREQ(UR_SET_FEATURE, UT_WRITE_DEVICE):
		DPRINTF(("unet: (UR_SET_FEATURE, UT_WRITE_DEVICE) %x", value));
		break;

	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_INTERFACE):
	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_ENDPOINT):
	case UREQ(UR_SET_FEATURE, UT_WRITE_INTERFACE):
	case UREQ(UR_SET_FEATURE, UT_WRITE_ENDPOINT):
		DPRINTF(("unet: (UR_CLEAR_FEATURE, UT_WRITE_INTERFACE)"));
		// err = USB_ERR_STALLED;
		goto done;

	case UREQ(UR_SET_INTERFACE, UT_WRITE_INTERFACE):
		DPRINTF(("unet set interface %u", value));
		break;

	case UREQ(UR_ISOCH_DELAY, UT_WRITE_DEVICE):
		DPRINTF(("unet set isoch delay %u", value));
		break;

	case UREQ(UR_SET_SEL, 0):
		DPRINTF(("unet set sel"));
		break;

	case UREQ(UR_SYNCH_FRAME, UT_WRITE_ENDPOINT):
		DPRINTF(("unet synch frame"));
		break;

		/* device specific requests */
	case UREQ(UCDC_NCM_GET_NTB_PARAMETERS, UT_READ_CLASS_INTERFACE):
		DPRINTF((
		    "unet: (UCDC_NCM_GET_NTB_PARAMETERS, UT_READ_CLASS_INTERFACE)"));
		if (!data)
			break;

		if (len > sizeof(ncm_params)) {
			data->blen = len - sizeof(ncm_params);
			len = sizeof(ncm_params);
		} else
			data->blen = 0;
		memcpy(data->buf, &ncm_params, len);
		data->bdone += len;
		break;

	case UREQ(UCDC_NCM_GET_MAX_DATAGRAM_SIZE, UT_READ_CLASS_INTERFACE):
		if (!data)
			break;

		if (len >= 2) {
			data->blen = len - 2;
			len = 2;
		} else
			data->blen = 0;
		memcpy(data->buf, &sc->max_dgram, len);
		data->bdone += len;
		break;

	case UREQ(UCDC_NCM_GET_NTB_FORMAT, UT_READ_CLASS_INTERFACE):
		if (!data)
			break;

		if (len >= 2) {
			data->blen = len - 2;
			len = 2;
		} else
			data->blen = 0;
		memcpy(data->buf, &sc->ntb_format, len);
		data->bdone += len;
		break;

	case UREQ(UCDC_NCM_GET_NTB_INPUT_SIZE, UT_READ_CLASS_INTERFACE):
		if (!data)
			break;

		if (len >= 4) {
			data->blen = len - 4;
			len = 4;
		} else
			data->blen = 0;
		memcpy(data->buf, &sc->ntb_input_size, len);
		data->bdone += len;
		break;

	case UREQ(UCDC_NCM_SET_NTB_FORMAT, UT_WRITE_CLASS_INTERFACE):
		DPRINTF((
		    "unet: (UCDC_NCM_SET_NTB_FORMAT, UT_WRITE_CLASS_INTERFACE): %d",
		    value));
		if (!unet_ntb_format_supported(value)) {
			err = USB_ERR_STALLED;
			goto done;
		}
		sc->ntb_format = value;
		break;

	case UREQ(UCDC_NCM_SET_NTB_INPUT_SIZE, UT_WRITE_CLASS_INTERFACE):
		DPRINTF((
		    "unet: (UCDC_NCM_SET_NTB_INPUT_SIZE, UT_WRITE_CLASS_INTERFACE)"));
		if (!data || len < 4)
			break;
		sc->ntb_input_size = usb_extract_u32(data->buf);
		if (sc->ntb_input_size < sizeof(struct usb_ncm16_hdr) +
		    sizeof(struct usb_ncm16_dpt) +
		    (2 * sizeof(struct usb_ncm16_dp)) ||
		    sc->ntb_input_size > UNET_NTB_MAX_SIZE) {
			err = USB_ERR_STALLED;
			goto done;
		}
		break;

	case UREQ(UCDC_NCM_SET_MAX_DATAGRAM_SIZE, UT_WRITE_CLASS_INTERFACE):
		DPRINTF((
		    "unet: (UCDC_NCM_SET_MAX_DATAGRAM_SIZE, UT_WRITE_CLASS_INTERFACE)"));
		if (!data || len < 2)
			break;
		sc->max_dgram = usb_extract_u16(data->buf);
		break;

	case UREQ(UCDC_NCM_SET_ETHERNET_PACKET_FILTER, UT_READ_CLASS_INTERFACE):
		DPRINTF((
		    "unet: (UCDC_NCM_SET_ETHERNET_PACKET_FILTER, UT_READ_CLASS_INTERFACE)"));
		err = USB_ERR_STALLED;
		goto done;

	case UREQ(UCDC_NCM_SET_ETHERNET_PACKET_FILTER,
	    UT_WRITE_CLASS_INTERFACE):
		DPRINTF((
		    "unet: (UCDC_NCM_SET_ETHERNET_PACKET_FILTER, UT_WRITE_CLASS_INTERFACE)"));
		break;

	default:
		DPRINTF(("unet: unknown(%d)->ERROR", xfer->ureq->bRequest));
		break;
	}
done:
	if (xfer->ureq && (xfer->ureq->bmRequestType & UT_WRITE) &&
	    (err == USB_ERR_NORMAL_COMPLETION) && (data != NULL))
		data->blen = 0;
	else if (eshort)
		err = USB_ERR_SHORT_XFER;

	pthread_mutex_unlock(&sc->mtx);

	DPRINTF(("unet request error code %d (0=ok), blen %u txlen %u", err,
	    (data ? data->blen : 0), (data ? data->bdone : 0)));

	return (err);
}

struct usb_cdc_notification speed_notify = {
	.bmRequestType = UCDC_NOTIFICATION,
	.bNotification = UCDC_N_CONNECTION_SPEED_CHANGE,
	.wValue = { 0 },
	.wIndex[0] = 1, // Interface 1
	.wLength[0] = 8,
	MSETQW(.data, 1lu * 1024 * 1024 * 1024, // Upstream
		1lu * 1024 * 1024 * 1024) // Downstream
};

struct usb_cdc_notification link_notify = {
	.bmRequestType = UCDC_NOTIFICATION,
	.bNotification = UCDC_N_NETWORK_CONNECTION,
	.wValue[0] = 1,
	.wIndex[0] = 1,
	.wLength = { 0 },
	MSETW(.data, 1),
};

static size_t
unet_process_bulk_out_packet(struct unet_softc *sc, struct iovec *iov,
    int iovcnt)
{
	uint16_t idx, pktlen;
	int nframes;
	struct iovec pktiov[32];
	struct usb_ncm16_hdr ntb;
	struct usb_ncm16_dpt ndp;
	struct usb_ncm16_dp dp[32];
	int pktiovcnt;
	size_t avail;
	size_t advance;
	size_t copied;
	uint16_t block_len;
	uint16_t dpt_idx;
	uint16_t dpt_len;
	size_t consumed;
	size_t len;

	consumed = 0;
	while (iovcnt) {
		avail = count_iov(iov, iovcnt);
		if (avail < sizeof(ntb)) {
			DPRINTF(("unet bulk-out: partial NTB tail %zu byte(s) left in TRB",
			    avail));
			break;
		}

		copied = iov_copy(&ntb, sizeof(ntb), iov, iovcnt, 0);
		if (copied < sizeof(ntb)) {
			WPRINTF(("unet bulk-out: short NTB header copy %zu/%zu byte(s)",
			    copied, sizeof(ntb)));
			break;
		}

		if (usb_extract_u32(ntb.dwSignature) != 0x484D434E) { // "NCMH"
			EPRINTLN("Invalid NTB signature %x, len %d",
			    usb_extract_u32(ntb.dwSignature),
			    usb_extract_u16(ntb.wHeaderLength));
			iov = iov_trim(iov, &iovcnt, 1);
			consumed += 1;
			continue;
		}

		block_len = usb_extract_u16(ntb.wBlockLength);
		dpt_idx = usb_extract_u16(ntb.wDptIndex);
		if (block_len > avail) {
			WPRINTF(("unet bulk-out: NTB crosses TRB boundary: block=%u avail=%zu",
			    block_len, avail));
			break;
		}
		if (usb_extract_u16(ntb.wHeaderLength) < sizeof(ntb) ||
		    block_len < sizeof(ntb) ||
		    dpt_idx < sizeof(ntb) || dpt_idx + sizeof(ndp) > block_len) {
			EPRINTLN("Invalid NTB layout hdr=%u block=%u dpt=%u avail=%zu",
			    usb_extract_u16(ntb.wHeaderLength), block_len, dpt_idx,
			    avail);
			iov = iov_trim(iov, &iovcnt, 1);
			consumed += 1;
			continue;
		}

		copied = iov_copy(&ndp, sizeof(ndp), iov, iovcnt, dpt_idx);
		if (copied < sizeof(ndp)) {
			iov = iov_trim(iov, &iovcnt, 1);
			consumed += 1;
			continue;
		}

		if (usb_extract_u32(ndp.dwSignature) != 0x304D434E) { // "NCM0"
			EPRINTLN("Invalid NDP signature %d",
			    usb_extract_u32(ndp.dwSignature));
			iov = iov_trim(iov, &iovcnt, 1);
			consumed += 1;
			continue;
		}

		dpt_len = usb_extract_u16(ndp.wLength);
		if (dpt_len < sizeof(ndp) || dpt_idx + dpt_len > block_len) {
			EPRINTLN("Invalid NDP length %u", dpt_len);
			iov = iov_trim(iov, &iovcnt, 1);
			consumed += 1;
			continue;
		}

		nframes = (dpt_len - sizeof(ndp)) / sizeof(dp[0]);
		if (nframes <= 0) {
			iov = iov_trim(iov, &iovcnt, block_len);
			consumed += block_len;
			continue;
		}
		nframes = MIN(nframes, (int)nitems(dp));
		copied = iov_copy(dp, nframes * sizeof(dp[0]), iov, iovcnt,
		    dpt_idx + sizeof(ndp));
		if (copied < (size_t)nframes * sizeof(dp[0])) {
			iov = iov_trim(iov, &iovcnt, 1);
			consumed += 1;
			continue;
		}

		for (int i = 0; i < nframes; i++) {
			idx = usb_extract_u16(dp[i].wFrameIndex);
			pktlen = usb_extract_u16(dp[i].wFrameLength);
			if (!idx || !pktlen) {
				break;
			}
			if ((size_t)idx + pktlen > block_len) {
				EPRINTLN("Invalid frame range idx=%u len=%u block=%u",
				    idx, pktlen, block_len);
				break;
			}

			len = make_iov(iov, iovcnt, pktiov, &pktiovcnt, idx,
			    pktlen);
			if (len != pktlen) {
				EPRINTLN("Short frame gather %zu/%u", len, pktlen);
				break;
			}

			unet_transmit_backend(sc, pktiov, pktiovcnt);
		}

		advance = block_len;
		iov = iov_trim(iov, &iovcnt, advance);
		consumed += advance;
	}

	return (consumed);
}

static int
unet_process_bulk_out(struct unet_softc *sc, struct usb_data_xfer *xfer)
{
	struct usb_data_xfer_block *data;
	int i, idx;
	int err = USB_ERR_NORMAL_COMPLETION;
	struct iovec iov[USB_MAX_XFER_BLOCKS];
	size_t consumed, remaining;
	int iovcnt;

	iovcnt = 0;
	idx = xfer->head;
	for (i = 0; i < xfer->ndata; i++) {
		data = &xfer->data[idx];
		if (data->processed) {
			idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
			continue;
		}

		if (data->buf != NULL && data->blen != 0) {
			iov[iovcnt].iov_base = (uint8_t *)data->buf + data->bdone;
			iov[iovcnt].iov_len = data->blen;
			iovcnt++;
		} else {
			data->processed = 1;
		}
		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
	}

	if (iovcnt == 0)
		return (err);

	consumed = unet_process_bulk_out_packet(sc, iov, iovcnt);
	remaining = consumed;

	idx = xfer->head;
	for (i = 0; i < xfer->ndata && remaining > 0; i++) {
		size_t chunk;

		data = &xfer->data[idx];
		if (data->processed) {
			idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
			continue;
		}
		if (data->buf == NULL || data->blen == 0) {
			data->processed = 1;
			idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
			continue;
		}

		chunk = MIN((size_t)data->blen, remaining);
		data->bdone += chunk;
		data->blen -= chunk;
		remaining -= chunk;
		if (data->blen == 0)
			data->processed = 1;

		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
	}

	return err;
}

static int
unet_process_bulk_in(struct unet_softc *sc, struct usb_data_xfer *xfer)
{
	struct usb_data_xfer_block *data;
	int idx, i;
	int err = USB_ERR_SHORT_XFER;
	struct iovec iov[USB_MAX_XFER_BLOCKS];
	int iovcnt;
	struct usb_ncm16_hdr *ntb;
	struct usb_ncm16_dpt *dpt;
	struct usb_ncm16_dp *dp;
	size_t len, total_len, plen;
	struct iovec *riov;
	int riov_len, hdr_len, pkt;

	DPRINTF(("unet_process_bulk_in"));

	data = NULL;
	idx = xfer->head;
	iovcnt = 0;
	for (i = 0; i < xfer->ndata && iovcnt < nitems(iov); i++) {
		data = &xfer->data[idx];
		if (data->processed) {
			idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
			continue;
		}

		if (data->buf != NULL && data->blen != 0) {
			iov->iov_base = data->buf;
			iov->iov_len = data->blen;
			DPRINTF(("xfer block size %d, %d out of %d\n",
			    data->blen, i, xfer->ndata));
		} else {
			assert(i == xfer->ndata - 1);
			assert(0);
		}
		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;

		ntb = (struct usb_ncm16_hdr *)iov->iov_base;
		dpt = (struct usb_ncm16_dpt *)(ntb + 1);
		dp = (struct usb_ncm16_dp *)(dpt + 1);

		riov = iov;
		riov_len = 1;
		hdr_len = sizeof(struct usb_ncm16_hdr) +
		    sizeof(struct usb_ncm16_dpt) +
		    8 * sizeof(struct usb_ncm16_dp) + 4;
		riov = iov_trim(riov, &riov_len, hdr_len);
		if (riov == NULL) {
			WPRINTF(
			    ("unet_process_bulk_in: not enough header space"));
			WPRINTF(("xfer block size %d, %d out of %d\n",
			    data->blen, i, xfer->ndata));
			assert(0);
			return (err);
		}

		total_len = hdr_len;
		pkt = 0;
		while (riov && riov->iov_len > UNET_MTU_SIZE && pkt < 8) {
			len = netbe_recv(sc->vsc_be, riov, riov_len);
			if (len <= 0)
				break;

			USETW(dp[pkt].wFrameIndex, total_len);
			USETW(dp[pkt].wFrameLength, len);
			total_len += len;

			riov = iov_trim(riov, &riov_len, len);
			pkt++;
		}

		if (total_len == hdr_len)
			break;

		plen = data->blen;
		len = hdr_len;
		data->bdone = plen;
		data->blen = 0;
		data->processed = 1;

		USETW(dp[pkt].wFrameIndex, 0);
		USETW(dp[pkt].wFrameLength, 0);

		ntb->dwSignature[0] = 'N';
		ntb->dwSignature[1] = 'C';
		ntb->dwSignature[2] = 'M';
		ntb->dwSignature[3] = 'H';
		USETW(ntb->wHeaderLength, sizeof(*ntb));
		USETW(ntb->wBlockLength, total_len);
		USETW(ntb->wSequence, sc->seq++);
		USETW(ntb->wDptIndex, sizeof(*ntb));

		dpt->dwSignature[0] = 'N';
		dpt->dwSignature[1] = 'C';
		dpt->dwSignature[2] = 'M';
		dpt->dwSignature[3] = '0';
		USETW(dpt->wNextNdpIndex, 0);
		USETW(dpt->wLength, sizeof(*dpt) + (4 * pkt) + 4);
	}

	return (err);
}

static int
unet_data_handler(void *scarg, struct usb_data_xfer *xfer, int dir, int epctx)
{
	struct unet_softc *sc;
	struct usb_data_xfer_block *data;
	uint8_t *udata;
	int len, i, idx;
	int err;

	sc = scarg;
	err = USB_ERR_NORMAL_COMPLETION;

	DPRINTF(("unet handle data - DIR=%s|EP=%d, blen %d", dir ? "IN" : "OUT",
	    epctx, xfer->data[0].blen));

	if (epctx == 2)
		return unet_process_bulk_out(sc, xfer);
	if (epctx == 3)
		return unet_process_bulk_in(sc, xfer);

	/* handle xfer at first unprocessed item with buffer */
	data = NULL;
	idx = xfer->head;
	for (i = 0; i < xfer->ndata; i++) {
		data = &xfer->data[idx];
		if (data->buf != NULL && data->blen != 0) {
			break;
		} else {
			data->processed = 1;
			data = NULL;
		}
		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
	}
	if (!data)
		goto done;

	udata = data->buf;
	len = data->blen;

	if (udata == NULL) {
		DPRINTF(("ukbd no buffer provided for input"));
		err = USB_ERR_NOMEM;
		goto done;
	}

	data->processed = 1;
	if (epctx == UNET_NOTIFY_ENDPT) {
		switch (sc->notify_state) {
		case UNET_STATE_NOTIFY_LINK_SPEED:
			DPRINTF(
			    ("unet_data_handler: notify link speed %d", len));
			if (len > 16) {
				data->blen = len - 16;
				len = 16;
			} else
				data->blen = 0;
			memcpy(udata, &speed_notify, len);
			data->bdone += len;
			udata += len;
			sc->notify_state = UNET_STATE_NOTIFY_LINK_UP;
			len = data->blen;
			if (len == 0)
				break;
			// fallthrough
		case UNET_STATE_NOTIFY_LINK_UP:
			DPRINTF(("unet_data_handler: notify link up"));
			if (len > 8) {
				data->blen = len - 8;
				len = 8;
			} else
				data->blen = 0;
			memcpy(udata, &link_notify, len);
			data->bdone += len;
			sc->notify_state = UNET_STATE_NOTIFY_IDLE;

			netbe_rx_enable(sc->vsc_be);
			break;

		default:
			err = USB_ERR_CANCELLED;
			USB_DATA_SET_ERRCODE(&xfer->data[xfer->head], USB_NAK);
			goto done;
		}
	}

	if (data->blen > 0)
		err = USB_ERR_SHORT_XFER;

done:
	return (err);
}

static int
unet_reset(void *scarg)
{
	return (0);
}

static int
unet_remove(void *scarg __unused)
{
	return (0);
}

static int
unet_stop(void *scarg __unused)
{
	return (0);
}

static struct usb_devemu ue_net = {
	.ue_emu = "net",
	.ue_usbver = 3,
	.ue_usbspeed = USB_SPEED_SUPER,
	.ue_init = unet_init,
	.ue_request = unet_request,
	.ue_data = unet_data_handler,
	.ue_reset = unet_reset,
	.ue_remove = unet_remove,
	.ue_stop = unet_stop,
#ifdef BHYVE_SNAPSHOT
	.ue_snapshot = umouse_snapshot,
#endif
};
USB_EMUL_SET(ue_net);
