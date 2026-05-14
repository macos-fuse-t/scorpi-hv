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

#include <support/freebsd_compat.h>
#include <sys/time.h>
#include "common.h"

// #include <machine/vmm_snapshot.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb.h"
#include "usbdi.h"

#include "bhyvegc.h"
#include "console.h"
#include "debug.h"
#include "usb_emul.h"

static int ukbd_debug = 0;
#define DPRINTF(params) \
	if (ukbd_debug) \
	PRINTLN params
#define WPRINTF(params)	      PRINTLN params

#define UKBD_INTR_ENDPT	      1
#define UKBD_REPORT_DESC_TYPE 0x22

#define HSETW(ptr, val)	      ptr = { (uint8_t)(val), (uint8_t)((val) >> 8) }

#define UKBD_GET_REPORT	      0x01
#define UKBD_GET_IDLE	      0x02
#define UKBD_GET_PROTOCOL     0x03
#define UKBD_SET_REPORT	      0x09
#define UKBD_SET_IDLE	      0x0A
#define UKBD_SET_PROTOCOL     0x0B

enum {
	UMSTR_LANG,
	UMSTR_MANUFACTURER,
	UMSTR_PRODUCT,
	UMSTR_SERIAL,
	UMSTR_CONFIG,
	UMSTR_MAX
};

static const char *ukbd_desc_strings[] = {
	"\x09\x04", // english
	"SCORPI",
	"HID Keyboard",
	"01",
	"HID Keyboard Device",
};

#define MSETW(ptr, val) ptr = { (uint8_t)(val), (uint8_t)((val) >> 8) }

static const uint8_t ukbd_report_desc[] = {
	0x05, 0x01, /* USAGE_PAGE (Generic Desktop)        */
	0x09, 0x06, /* USAGE (Keyboard)                   */
	0xA1, 0x01, /* COLLECTION (Application)           */
	0x05, 0x07, /*   USAGE_PAGE (Keyboard/Keypad)     */
	0x19, 0xE0, /*   USAGE_MINIMUM (Left Control)     */
	0x29, 0xE7, /*   USAGE_MAXIMUM (Right GUI)        */
	0x15, 0x00, /*   LOGICAL_MINIMUM (0)              */
	0x25, 0x01, /*   LOGICAL_MAXIMUM (1)              */
	0x75, 0x01, /*   REPORT_SIZE (1)                  */
	0x95, 0x08, /*   REPORT_COUNT (8)                 */
	0x81, 0x02, /*   INPUT (Data,Var,Abs)             */
	0x95, 0x01, /*   REPORT_COUNT (1)                 */
	0x75, 0x08, /*   REPORT_SIZE (8)                  */
	0x81, 0x01, /*   INPUT (Constant) Reserved byte   */
	0x95, 0x06, /*   REPORT_COUNT (6)                 */
	0x75, 0x08, /*   REPORT_SIZE (8)                  */
	0x15, 0x00, /*   LOGICAL_MINIMUM (0)              */
	0x25, 0x65, /*   LOGICAL_MAXIMUM (101)            */
	0x05, 0x07, /*   USAGE_PAGE (Keyboard/Keypad)     */
	0x19, 0x00, /*   USAGE_MINIMUM (0)                */
	0x29, 0x65, /*   USAGE_MAXIMUM (101)              */
	0x81, 0x00, /*   INPUT (Data,Array) Keycodes      */
	0xC0	    /* END_COLLECTION                     */
};

// HID descriptor for a keyboard
struct ukbd_hid_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bcdHID[2];
	uint8_t bCountryCode;
	uint8_t bNumDescriptors;
	uint8_t bReportDescriptorType;
	uint8_t wItemLength[2];
} __packed;

// USB configuration descriptor for a keyboard
struct ukbd_config_desc {
	struct usb_config_descriptor confd;
	struct usb_interface_descriptor ifcd;
	struct ukbd_hid_descriptor hidd;
	struct usb_endpoint_descriptor endpd;
	struct usb_endpoint_ss_comp_descriptor sscompd;
} __packed;

struct ukbd_config_desc ukbd_confd = {
    .confd = {
        .bLength = sizeof(struct usb_config_descriptor),
        .bDescriptorType = UDESC_CONFIG, // Configuration Descriptor
        .wTotalLength[0] = sizeof(ukbd_confd),
        .bNumInterface = 1,
        .bConfigurationValue = 1,
        .iConfiguration = UMSTR_CONFIG,
        .bmAttributes = UC_BUS_POWERED | UC_REMOTE_WAKEUP, // bus-powered, remote wakeup
        .bMaxPower = 0x32, // 100 mA
    },
    .ifcd = {
        .bLength = sizeof(struct usb_interface_descriptor),
        .bDescriptorType = UDESC_INTERFACE, // Interface Descriptor
        .bInterfaceNumber = 0,
        .bAlternateSetting = 0,
        .bNumEndpoints = 1,
        .bInterfaceClass = UICLASS_HID, // HID
        .bInterfaceSubClass = UISUBCLASS_BOOT, // Boot Interface Subclass
        .bInterfaceProtocol = UIPROTO_BOOT_KEYBOARD, // Keyboard
        .iInterface = 0,
    },
    .hidd = {
        .bLength = sizeof(struct ukbd_hid_descriptor),
        .bDescriptorType = 0x21, // HID Descriptor
        .bcdHID = {0x01, 0x01}, // HID Version 1.1
        .bCountryCode = 0, // Not localized
        .bNumDescriptors = 1,
        .bReportDescriptorType = UKBD_REPORT_DESC_TYPE, // Report Descriptor
        .wItemLength = {sizeof(ukbd_report_desc), 0x00}, // Report descriptor length
    },
    .endpd = {
        .bLength = sizeof(struct usb_endpoint_descriptor),
        .bDescriptorType = UDESC_ENDPOINT, // Endpoint Descriptor
        .bEndpointAddress = UE_DIR_IN | UKBD_INTR_ENDPT, // IN endpoint
        .bmAttributes = UE_INTERRUPT, // Interrupt
        .wMaxPacketSize[0] = 0x8, // 8 bytes
        .bInterval = 0x07, // 8 ms polling interval
    },
	.sscompd = {
		.bLength = sizeof(struct usb_endpoint_ss_comp_descriptor),
		.bDescriptorType = UDESC_ENDPOINT_SS_COMP,
		.bMaxBurst = 0,
		.bmAttributes = 0,
		MSETW(.wBytesPerInterval, 8),
	},
};

struct ukbd_bos_desc {
	struct usb_bos_descriptor bosd; // BOS descriptor
	struct usb_devcap_ss_descriptor
	    usbssd; // SuperSpeed device capability descriptor
} __packed;

static struct ukbd_bos_desc ukbd_bosd = {
    .bosd = {
        .bLength = sizeof(ukbd_bosd.bosd),
        .bDescriptorType = UDESC_BOS,
        .wTotalLength[0] = sizeof(ukbd_bosd),
        .bNumDeviceCaps = 1,
    },
    .usbssd = {
        .bLength = sizeof(ukbd_bosd.usbssd),
        .bDescriptorType = UDESC_DEVICE_CAPABILITY,
        .bDevCapabilityType = 0x03, // SuperSpeed capability type
        .bmAttributes = 0x00,   // No special attributes
        HSETW(.wSpeedsSupported, 0x0e), // Supported speeds: FS (bit 1), HS (bit 2), SS (bit 3)
        .bFunctionalitySupport = 0x03, // Full functionality at SuperSpeed
        .bU1DevExitLat = 0x0A,  // U1 exit latency (dummy, not used)
        .wU2DevExitLat = { 0x20, 0x00 }, // U2 exit latency (dummy, not used)
    },
};

static struct usb_device_descriptor ukbd_dev_desc = {
	.bLength = sizeof(ukbd_dev_desc),
	.bDescriptorType = UDESC_DEVICE,
	MSETW(.bcdUSB, UD_USB_3_0),
	.bMaxPacketSize = 9,	   /* max pkt size, 2^9 = 512 */
	MSETW(.idVendor, 0xFB5D),  /* vendor */
	MSETW(.idProduct, 0x0002), /* product */
	MSETW(.bcdDevice, 0),	   /* device version */
	.iManufacturer = UMSTR_MANUFACTURER,
	.iProduct = UMSTR_PRODUCT,
	.iSerialNumber = UMSTR_SERIAL,
	.bNumConfigurations = 1,
};

struct ukbd_report {
	uint8_t modifiers; /* Modifier keys: Ctrl, Shift, Alt, GUI */
	uint8_t reserved;  /* Reserved byte                        */
	uint8_t keys[6];   /* Keycode array: Up to 6 key presses   */
} __packed;

struct ukbd_softc {
	struct usb_hci *hci;

	struct ukbd_report um_report;
	int newdata;
	struct {
		uint8_t idle;
		uint8_t protocol;
		uint8_t feature;
	} hid;

	pthread_mutex_t mtx;
	pthread_mutex_t ev_mtx;
	int polling;
	struct timeval prev_evt;
};

static void
ukbd_event(int down, uint8_t hidcode, uint8_t modifiers, void *arg)
{
	struct ukbd_softc *sc;

	sc = arg;
	pthread_mutex_lock(&sc->mtx);

	if (down) {
		int free_index = -1;
		for (int i = 0; i < 6; i++) {
			if (sc->um_report.keys[i] == hidcode)
				break;
			if (!sc->um_report.keys[i] && free_index == -1)
				free_index = i;
		}
		if (free_index != -1)
			sc->um_report.keys[free_index] = hidcode;
	} else {
		for (int i = 0; i < 6; i++) {
			if (sc->um_report.keys[i] == hidcode) {
				sc->um_report.keys[i] = 0;
				break;
			}
		}
	}

	sc->um_report.modifiers = modifiers;
	sc->newdata = 1;
	pthread_mutex_unlock(&sc->mtx);

	// pthread_mutex_lock(&sc->ev_mtx);
	sc->hci->hci_intr(sc->hci, UE_DIR_IN | UKBD_INTR_ENDPT);
	// pthread_mutex_unlock(&sc->ev_mtx);
}

static void *
ukbd_init(struct usb_hci *hci, nvlist_t *nvl __unused)
{
	struct ukbd_softc *sc;

	sc = calloc(1, sizeof(struct ukbd_softc));
	sc->hci = hci;

	sc->hid.protocol = 1; /* REPORT protocol */
	pthread_mutex_init(&sc->mtx, NULL);
	pthread_mutex_init(&sc->ev_mtx, NULL);

	console_kbd_register(ukbd_event, sc, 10);

	return (sc);
}

#define UREQ(x, y) ((x) | ((y) << 8))

static int
ukbd_request(void *scarg, struct usb_data_xfer *xfer)
{
	struct ukbd_softc *sc;
	struct usb_data_xfer_block *data;
	const char *str;
	uint16_t value;
	uint16_t index;
	uint16_t len = 0;
	uint16_t slen;
	uint8_t *udata;
	int err;
	int i, idx;
	int eshort;

	sc = scarg;

	data = NULL;
	udata = NULL;
	idx = xfer->head;
	// printf("xfer->ndata %d\n", xfer->ndata);
	for (i = 0; i < xfer->ndata; i++) {
		// printf("ndata %d - blen %d, bdone %d, processed %d\n",
		//  i, USB_DATA_OK(xfer,i)? xfer->data[idx].blen:0,
		//  USB_DATA_OK(xfer,i)?xfer->data[idx].bdone:0,
		//  USB_DATA_OK(xfer,i)?xfer->data[idx].processed:0);
		xfer->data[idx].bdone = 0;
		if (data == NULL && USB_DATA_OK(xfer, i)) {
			data = &xfer->data[idx];
			udata = data->buf;
		}

		xfer->data[idx].processed = 1;
		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
	}
	// printf("data %p\n", data);

	err = USB_ERR_NORMAL_COMPLETION;
	eshort = 0;

	if (!xfer->ureq) {
		DPRINTF(("ukbd_request: port %d", sc->hci->hci_port));
		goto done;
	}

	value = UGETW(xfer->ureq->wValue);
	index = UGETW(xfer->ureq->wIndex);
	len = UGETW(xfer->ureq->wLength);

	DPRINTF(("ukbd_request: port %d, type 0x%x, req 0x%x, val 0x%x, "
		 "idx 0x%x, len %u",
	    sc->hci->hci_port, xfer->ureq->bmRequestType, xfer->ureq->bRequest,
	    value, index, len));

	switch (UREQ(xfer->ureq->bRequest, xfer->ureq->bmRequestType)) {
	case UREQ(UR_GET_CONFIG, UT_READ_DEVICE):
		DPRINTF(("ukbd: (UR_GET_CONFIG, UT_READ_DEVICE)"));
		if (!data)
			break;

		*udata = ukbd_confd.confd.bConfigurationValue;
		data->blen = len > 0 ? len - 1 : 0;
		eshort = data->blen > 0;
		data->bdone += 1;
		break;

	case UREQ(UR_GET_DESCRIPTOR, UT_READ_DEVICE):
		DPRINTF((
		    "ukbd: (UR_GET_DESCRIPTOR, UT_READ_DEVICE) val %x, data %d",
		    value >> 8, data != NULL));
		if (!data)
			break;

		switch (value >> 8) {
		case UDESC_DEVICE:
			DPRINTF(("ukbd: (->UDESC_DEVICE) len %u ?= "
				 "sizeof(ukbd_dev_desc) %lu",
			    len, sizeof(ukbd_dev_desc)));
			if ((value & 0xFF) != 0) {
				err = USB_ERR_STALLED;
				goto done;
			}
			if (len > sizeof(ukbd_dev_desc)) {
				data->blen = len - sizeof(ukbd_dev_desc);
				len = sizeof(ukbd_dev_desc);
			} else
				data->blen = 0;
			memcpy(data->buf, &ukbd_dev_desc, len);
			data->bdone += len;
			break;

		case UDESC_CONFIG:
			DPRINTF(("ukbd: (->UDESC_CONFIG)"));
			if ((value & 0xFF) != 0) {
				err = USB_ERR_STALLED;
				goto done;
			}
			// rintf("len %d, sizeof(ukbd_confd) %lu!!!\n", len,
			// sizeof(ukbd_confd));
			if (len > sizeof(ukbd_confd)) {
				data->blen = len - sizeof(ukbd_confd);
				len = sizeof(ukbd_confd);
			} else
				data->blen = 0;

			memcpy(data->buf, &ukbd_confd, len);
			data->bdone += len;
			break;

		case UDESC_STRING:
			DPRINTF(("ukbd: (->UDESC_STRING) %d", value));
			str = NULL;
			if ((value & 0xFF) < UMSTR_MAX)
				str = ukbd_desc_strings[value & 0xFF];
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
			for (i = 2; i < len - 1; i += 2) {
				udata[i] = *str++;
				udata[i + 1] = '\0';
			}
			data->bdone += len;

			break;

		case UDESC_BOS:
			DPRINTF(("ukbd: USB3 BOS"));
			if (len > sizeof(ukbd_bosd)) {
				data->blen = len - sizeof(ukbd_bosd);
				len = sizeof(ukbd_bosd);
			} else
				data->blen = 0;
			memcpy(udata, &ukbd_bosd, len);
			data->bdone += len;
			break;

		default:
			DPRINTF(("ukbd: unknown(%d)->ERROR", value >> 8));
			err = USB_ERR_STALLED;
			goto done;
		}
		if (data)
			eshort = data->blen > 0;
		break;

	case UREQ(UR_GET_DESCRIPTOR, UT_READ_INTERFACE):
		DPRINTF(("ukbd: (UR_GET_DESCRIPTOR, UT_READ_INTERFACE) "
			 "0x%x",
		    (value >> 8)));
		if (!data)
			break;
		switch (value >> 8) {
		case UKBD_REPORT_DESC_TYPE:
			if (len > sizeof(ukbd_report_desc)) {
				data->blen = len - sizeof(ukbd_report_desc);
				len = sizeof(ukbd_report_desc);
			} else
				data->blen = 0;
			memcpy(data->buf, ukbd_report_desc, len);
			data->bdone += len;
			break;
		default:
			DPRINTF(("ukbd: IO ERROR"));
			err = USB_ERR_STALLED;
			goto done;
		}
		if (data)
			eshort = data->blen > 0;
		break;

	case UREQ(UR_GET_INTERFACE, UT_READ_INTERFACE):
		DPRINTF(("ukbd: (UR_GET_INTERFACE, UT_READ_INTERFACE)"));
		if (index != 0) {
			DPRINTF(
			    ("ukbd get_interface, invalid index %d", index));
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
		DPRINTF(("ukbd: (UR_GET_STATUS, UT_READ_DEVICE)"));
		if (data != NULL && len > 1) {
			if (sc->hid.feature == UF_DEVICE_REMOTE_WAKEUP)
				USETW(udata, UDS_REMOTE_WAKEUP);
			else
				USETW(udata, 0);
			data->blen = len - 2;
			data->bdone += 2;
		}
		if (data)
			eshort = data->blen > 0;
		break;

	case UREQ(UR_GET_STATUS, UT_READ_INTERFACE):
	case UREQ(UR_GET_STATUS, UT_READ_ENDPOINT):
		DPRINTF(("ukbd: (UR_GET_STATUS, UT_READ_INTERFACE)"));
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
		DPRINTF(("ukbd set address %u", value));
		break;

	case UREQ(UR_SET_CONFIG, UT_WRITE_DEVICE):
		DPRINTF(("ukbd set config %u", value));
		break;

	case UREQ(UR_SET_DESCRIPTOR, UT_WRITE_DEVICE):
		DPRINTF(("ukbd set descriptor %u", value));
		break;

	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_DEVICE):
		DPRINTF(("ukbd: (UR_SET_FEATURE, UT_WRITE_DEVICE) %x", value));
		if (value == UF_DEVICE_REMOTE_WAKEUP)
			sc->hid.feature = 0;
		break;

	case UREQ(UR_SET_FEATURE, UT_WRITE_DEVICE):
		DPRINTF(("ukbd: (UR_SET_FEATURE, UT_WRITE_DEVICE) %x", value));
		if (value == UF_DEVICE_REMOTE_WAKEUP)
			sc->hid.feature = UF_DEVICE_REMOTE_WAKEUP;
		break;

	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_INTERFACE):
	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_ENDPOINT):
	case UREQ(UR_SET_FEATURE, UT_WRITE_INTERFACE):
	case UREQ(UR_SET_FEATURE, UT_WRITE_ENDPOINT):
		DPRINTF(("ukbd: (UR_CLEAR_FEATURE, UT_WRITE_INTERFACE)"));
		err = USB_ERR_STALLED;
		goto done;

	case UREQ(UR_SET_INTERFACE, UT_WRITE_INTERFACE):
		DPRINTF(("ukbd set interface %u", value));
		break;

	case UREQ(UR_ISOCH_DELAY, UT_WRITE_DEVICE):
		DPRINTF(("ukbd set isoch delay %u", value));
		break;

	case UREQ(UR_SET_SEL, 0):
		DPRINTF(("ukbd set sel"));
		break;

	case UREQ(UR_SYNCH_FRAME, UT_WRITE_ENDPOINT):
		DPRINTF(("ukbd synch frame"));
		break;

		/* HID device requests */

	case UREQ(UKBD_GET_REPORT, UT_READ_CLASS_INTERFACE):
		DPRINTF(("ukbd: (UKBD_GET_REPORT, UT_READ_CLASS_INTERFACE) "
			 "0x%x",
		    (value >> 8)));
		if (!data)
			break;

		if ((value >> 8) == 0x01 && len >= sizeof(sc->um_report)) {
			/* TODO read from backend */
			if (len > sizeof(sc->um_report)) {
				data->blen = len - sizeof(sc->um_report);
				len = sizeof(sc->um_report);
			} else
				data->blen = 0;

			memcpy(data->buf, &sc->um_report, len);
			data->bdone += len;
		} else {
			err = USB_ERR_STALLED;
			goto done;
		}
		eshort = data->blen > 0;
		break;

	case UREQ(UKBD_GET_IDLE, UT_READ_CLASS_INTERFACE):
		if (data != NULL && len > 0) {
			*udata = sc->hid.idle;
			data->blen = len - 1;
			data->bdone += 1;
		}
		eshort = data->blen > 0;
		break;

	case UREQ(UKBD_GET_PROTOCOL, UT_READ_CLASS_INTERFACE):
		if (data != NULL && len > 0) {
			*udata = sc->hid.protocol;
			data->blen = len - 1;
			data->bdone += 1;
			eshort = data->blen > 0;
		}
		break;

	case UREQ(UKBD_SET_REPORT, UT_WRITE_CLASS_INTERFACE):
		DPRINTF((
		    "ukbd: (UKBD_SET_REPORT, UT_WRITE_CLASS_INTERFACE) ignored"));
		break;

	case UREQ(UKBD_SET_IDLE, UT_WRITE_CLASS_INTERFACE):
		sc->hid.idle = UGETW(xfer->ureq->wValue) >> 8;
		DPRINTF(("ukbd: (UKBD_SET_IDLE, UT_WRITE_CLASS_INTERFACE) %x",
		    sc->hid.idle));
		break;

	case UREQ(UKBD_SET_PROTOCOL, UT_WRITE_CLASS_INTERFACE):
		sc->hid.protocol = UGETW(xfer->ureq->wValue) >> 8;
		DPRINTF(
		    ("ukbd: (UR_CLEAR_FEATURE, UT_WRITE_CLASS_INTERFACE) %x",
			sc->hid.protocol));
		break;

	default:
		DPRINTF(("**** ukbd request unhandled"));
		err = USB_ERR_STALLED;
		break;
	}

done:
	if (xfer->ureq && (xfer->ureq->bmRequestType & UT_WRITE) &&
	    (err == USB_ERR_NORMAL_COMPLETION) && (data != NULL))
		data->blen = 0;
	else if (eshort)
		err = USB_ERR_SHORT_XFER;

	DPRINTF(("ukbd request error code %d (0=ok), blen %u txlen %u", err,
	    (data ? data->blen : 0), (data ? data->bdone : 0)));

	return (err);
}

static int
ukbd_data_handler(void *scarg, struct usb_data_xfer *xfer, int dir, int epctx)
{
	struct ukbd_softc *sc;
	struct usb_data_xfer_block *data;
	uint8_t *udata;
	int len, i, idx;
	int err;

	DPRINTF(("ukbd handle data - DIR=%s|EP=%d, blen %d", dir ? "IN" : "OUT",
	    epctx, xfer->data[0].blen));

	/* find buffer to add data */
	udata = NULL;
	err = USB_ERR_NORMAL_COMPLETION;

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

	sc = scarg;

	if (dir) {
		pthread_mutex_lock(&sc->mtx);

		if (!sc->newdata) {
			err = USB_ERR_CANCELLED;
			USB_DATA_SET_ERRCODE(&xfer->data[xfer->head], USB_NAK);
			pthread_mutex_unlock(&sc->mtx);
			goto done;
		}

		if (sc->polling) {
			err = USB_ERR_STALLED;
			USB_DATA_SET_ERRCODE(data, USB_STALL);
			pthread_mutex_unlock(&sc->mtx);
			goto done;
		}
		sc->polling = 1;

		if (len > 0) {
			sc->newdata = 0;

			data->processed = 1;
			data->bdone += sizeof(struct ukbd_report);
			memcpy(udata, &sc->um_report,
			    sizeof(struct ukbd_report));
			data->blen = len - sizeof(struct ukbd_report);
			if (data->blen > 0)
				err = USB_ERR_SHORT_XFER;
		}

		sc->polling = 0;
		pthread_mutex_unlock(&sc->mtx);
	} else {
		USB_DATA_SET_ERRCODE(data, USB_STALL);
		err = USB_ERR_STALLED;
	}

done:
	return (err);
}

static int
ukbd_reset(void *scarg)
{
	struct ukbd_softc *sc;

	sc = scarg;

	sc->newdata = 0;

	return (0);
}

static int
ukbd_remove(void *scarg __unused)
{
	return (0);
}

static int
ukbd_stop(void *scarg __unused)
{
	return (0);
}

#ifdef BHYVE_SNAPSHOT
static int
umouse_snapshot(void *scarg, struct vm_snapshot_meta *meta)
{
	int ret;
	struct umouse_softc *sc;

	sc = scarg;

	SNAPSHOT_VAR_OR_LEAVE(sc->um_report, meta, ret, done);
	SNAPSHOT_VAR_OR_LEAVE(sc->newdata, meta, ret, done);
	SNAPSHOT_VAR_OR_LEAVE(sc->hid.idle, meta, ret, done);
	SNAPSHOT_VAR_OR_LEAVE(sc->hid.protocol, meta, ret, done);
	SNAPSHOT_VAR_OR_LEAVE(sc->hid.feature, meta, ret, done);

	SNAPSHOT_VAR_OR_LEAVE(sc->polling, meta, ret, done);
	SNAPSHOT_VAR_OR_LEAVE(sc->prev_evt.tv_sec, meta, ret, done);
	SNAPSHOT_VAR_OR_LEAVE(sc->prev_evt.tv_usec, meta, ret, done);

done:
	return (ret);
}
#endif

static struct usb_devemu ue_kbd = {
	.ue_emu = "kbd",
	.ue_usbver = 3,
	.ue_usbspeed = USB_SPEED_HIGH,
	.ue_init = ukbd_init,
	.ue_request = ukbd_request,
	.ue_data = ukbd_data_handler,
	.ue_reset = ukbd_reset,
	.ue_remove = ukbd_remove,
	.ue_stop = ukbd_stop,
#ifdef BHYVE_SNAPSHOT
	.ue_snapshot = umouse_snapshot,
#endif
};
USB_EMUL_SET(ue_kbd);
