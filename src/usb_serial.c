/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 */

#include <support/freebsd_compat.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "debug.h"
#include "mevent.h"
#include "usb.h"
#include "usb_cdc.h"
#include "usbdi.h"
#include "usb_emul.h"

static int userial_debug = 0;
#define DPRINTF(params) \
	if (userial_debug) \
	PRINTLN params

#define MSETW(ptr, val) ptr = { (uint8_t)(val), (uint8_t)((val) >> 8) }
#define MSETDW(ptr, val)                                                    \
	ptr = { (uint8_t)(val), (uint8_t)((val) >> 8),                      \
		(uint8_t)((val) >> 16), (uint8_t)((val) >> 24) }

#define USERIAL_NOTIFY_ENDPT 1
#define USERIAL_BULK_OUT_ENDPT 2
#define USERIAL_BULK_IN_ENDPT 3
#define USERIAL_BULK_MPS 512

enum {
	USERIAL_STR_LANG,
	USERIAL_STR_MANUFACTURER,
	USERIAL_STR_PRODUCT,
	USERIAL_STR_SERIAL,
	USERIAL_STR_CONFIG,
	USERIAL_STR_COMM,
	USERIAL_STR_DATA,
	USERIAL_STR_MAX
};

static const char *userial_desc_strings[] = {
	"\x09\x04",
	"SCORPI",
	"Scorpi USB Serial Console",
	"01",
	"CDC ACM Serial",
	"CDC ACM Control",
	"CDC ACM Data",
};

struct userial_config_desc {
	struct usb_config_descriptor confd;
	struct usb_interface_assoc_descriptor iad;
	struct usb_interface_descriptor comm_ifcd;
	struct usb_cdc_header_descriptor cdc_header;
	struct usb_cdc_cm_descriptor cdc_cm;
	struct usb_cdc_acm_descriptor cdc_acm;
	struct usb_cdc_union_descriptor cdc_union;
	struct usb_endpoint_descriptor notify_ep;
	struct usb_interface_descriptor data_ifcd;
	struct usb_endpoint_descriptor bulk_out_ep;
	struct usb_endpoint_descriptor bulk_in_ep;
} __packed;

static struct userial_config_desc userial_confd = {
	.confd = {
		.bLength = sizeof(userial_confd.confd),
		.bDescriptorType = UDESC_CONFIG,
		MSETW(.wTotalLength, sizeof(userial_confd)),
		.bNumInterface = 2,
		.bConfigurationValue = 1,
		.iConfiguration = USERIAL_STR_CONFIG,
		.bmAttributes = UC_BUS_POWERED,
		.bMaxPower = 0x32,
	},
	.iad = {
		.bLength = sizeof(userial_confd.iad),
		.bDescriptorType = UDESC_IFACE_ASSOC,
		.bFirstInterface = 0,
		.bInterfaceCount = 2,
		.bFunctionClass = UICLASS_CDC,
		.bFunctionSubClass = UISUBCLASS_ABSTRACT_CONTROL_MODEL,
		.bFunctionProtocol = UIPROTO_CDC_NONE,
		.iFunction = USERIAL_STR_CONFIG,
	},
	.comm_ifcd = {
		.bLength = sizeof(userial_confd.comm_ifcd),
		.bDescriptorType = UDESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = UICLASS_CDC,
		.bInterfaceSubClass = UISUBCLASS_ABSTRACT_CONTROL_MODEL,
		.bInterfaceProtocol = UIPROTO_CDC_NONE,
		.iInterface = USERIAL_STR_COMM,
	},
	.cdc_header = {
		.bLength = sizeof(userial_confd.cdc_header),
		.bDescriptorType = UDESC_CS_INTERFACE,
		.bDescriptorSubtype = UDESCSUB_CDC_HEADER,
		MSETW(.bcdCDC, 0x0120),
	},
	.cdc_cm = {
		.bLength = sizeof(userial_confd.cdc_cm),
		.bDescriptorType = UDESC_CS_INTERFACE,
		.bDescriptorSubtype = UDESCSUB_CDC_CM,
		.bmCapabilities = 0,
		.bDataInterface = 1,
	},
	.cdc_acm = {
		.bLength = sizeof(userial_confd.cdc_acm),
		.bDescriptorType = UDESC_CS_INTERFACE,
		.bDescriptorSubtype = UDESCSUB_CDC_ACM,
		.bmCapabilities = USB_CDC_ACM_HAS_LINE |
		    USB_CDC_ACM_HAS_BREAK,
	},
	.cdc_union = {
		.bLength = sizeof(userial_confd.cdc_union),
		.bDescriptorType = UDESC_CS_INTERFACE,
		.bDescriptorSubtype = UDESCSUB_CDC_UNION,
		.bMasterInterface = 0,
		.bSlaveInterface = { 1 },
	},
	.notify_ep = {
		.bLength = sizeof(userial_confd.notify_ep),
		.bDescriptorType = UDESC_ENDPOINT,
		.bEndpointAddress = UE_DIR_IN | USERIAL_NOTIFY_ENDPT,
		.bmAttributes = UE_INTERRUPT,
		MSETW(.wMaxPacketSize, 16),
		.bInterval = 0x10,
	},
	.data_ifcd = {
		.bLength = sizeof(userial_confd.data_ifcd),
		.bDescriptorType = UDESC_INTERFACE,
		.bInterfaceNumber = 1,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = UICLASS_CDC_DATA,
		.bInterfaceSubClass = UISUBCLASS_DATA,
		.bInterfaceProtocol = UIPROTO_DATA_TRANSPARENT,
		.iInterface = USERIAL_STR_DATA,
	},
	.bulk_out_ep = {
		.bLength = sizeof(userial_confd.bulk_out_ep),
		.bDescriptorType = UDESC_ENDPOINT,
		.bEndpointAddress = UE_DIR_OUT | USERIAL_BULK_OUT_ENDPT,
		.bmAttributes = UE_BULK,
		MSETW(.wMaxPacketSize, USERIAL_BULK_MPS),
		.bInterval = 0,
	},
	.bulk_in_ep = {
		.bLength = sizeof(userial_confd.bulk_in_ep),
		.bDescriptorType = UDESC_ENDPOINT,
		.bEndpointAddress = UE_DIR_IN | USERIAL_BULK_IN_ENDPT,
		.bmAttributes = UE_BULK,
		MSETW(.wMaxPacketSize, USERIAL_BULK_MPS),
		.bInterval = 0,
	},
};

static struct usb_device_descriptor userial_dev_desc = {
	.bLength = sizeof(userial_dev_desc),
	.bDescriptorType = UDESC_DEVICE,
	MSETW(.bcdUSB, UD_USB_2_0),
	.bDeviceClass = UICLASS_CDC,
	.bDeviceSubClass = UISUBCLASS_ABSTRACT_CONTROL_MODEL,
	.bDeviceProtocol = UIPROTO_CDC_NONE,
	.bMaxPacketSize = 64,
	MSETW(.idVendor, 0xFB5D),
	MSETW(.idProduct, 0x0004),
	MSETW(.bcdDevice, 0x0001),
	.iManufacturer = USERIAL_STR_MANUFACTURER,
	.iProduct = USERIAL_STR_PRODUCT,
	.iSerialNumber = USERIAL_STR_SERIAL,
	.bNumConfigurations = 1,
};

struct userial_backend {
	int fd;
	int listen_fd;
	struct mevent *read_mev;
	struct mevent *listen_mev;
	char *unix_path;
};

struct userial_softc {
	struct usb_hci *hci;
	struct userial_backend backend;
	struct usb_cdc_line_state line_state;
	uint16_t control_line_state;
	bool configured;
	bool notify_pending;
	pthread_mutex_t mtx;
};

static void
userial_disconnect_locked(struct userial_softc *sc)
{
	if (sc->backend.read_mev != NULL) {
		mevent_delete(sc->backend.read_mev);
		sc->backend.read_mev = NULL;
	}
	if (sc->backend.fd >= 0) {
		close(sc->backend.fd);
		sc->backend.fd = -1;
	}
}

static void
userial_readable(int fd __unused, enum ev_type type __unused, void *arg)
{
	struct userial_softc *sc = arg;

	sc->hci->hci_intr(sc->hci, UE_DIR_IN | USERIAL_BULK_IN_ENDPT);
}

static void
userial_accept(int fd, enum ev_type type __unused, void *arg)
{
	static const char busy[] = "USB serial backend already connected\n";
	struct userial_softc *sc = arg;
	int conn_fd;

	conn_fd = accept(fd, NULL, NULL);
	if (conn_fd < 0)
		return;
	if (fcntl(conn_fd, F_SETFL, O_NONBLOCK) != 0) {
		close(conn_fd);
		return;
	}

	pthread_mutex_lock(&sc->mtx);
	if (sc->backend.fd >= 0) {
		(void)send(conn_fd, busy, sizeof(busy) - 1, 0);
		close(conn_fd);
		pthread_mutex_unlock(&sc->mtx);
		return;
	}

	sc->backend.fd = conn_fd;
	sc->backend.read_mev = mevent_add(conn_fd, EVF_READ, userial_readable,
	    sc);
	if (sc->backend.read_mev == NULL)
		userial_disconnect_locked(sc);
	pthread_mutex_unlock(&sc->mtx);
}

static int
userial_open_unix(struct userial_softc *sc, const char *path)
{
	const char *socket_path;
	struct sockaddr_un sun;
	int fd;

	socket_path = path + strlen("unix:");
	if (*socket_path == '\0') {
		warnx("USB serial unix backend requires a path");
		return (-1);
	}
	if (strlen(socket_path) >= sizeof(sun.sun_path)) {
		warnx("USB serial unix backend path too long: %s", socket_path);
		return (-1);
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return (-1);

	(void)unlink(socket_path);
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
#ifdef __APPLE__
	sun.sun_len = sizeof(sun);
#endif
	strlcpy(sun.sun_path, socket_path, sizeof(sun.sun_path));

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0 ||
	    fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || listen(fd, 1) < 0) {
		warn("USB serial unix backend %s", socket_path);
		close(fd);
		(void)unlink(socket_path);
		return (-1);
	}

	sc->backend.listen_fd = fd;
	sc->backend.unix_path = strdup(socket_path);
	if (sc->backend.unix_path == NULL) {
		close(fd);
		sc->backend.listen_fd = -1;
		(void)unlink(socket_path);
		return (-1);
	}
	sc->backend.listen_mev = mevent_add(fd, EVF_READ, userial_accept, sc);
	if (sc->backend.listen_mev == NULL) {
		close(fd);
		sc->backend.listen_fd = -1;
		(void)unlink(socket_path);
		free(sc->backend.unix_path);
		sc->backend.unix_path = NULL;
		return (-1);
	}
	return (0);
}

static int
userial_open_tcp(struct userial_softc *sc, const char *path)
{
	struct addrinfo hints, *src_addr;
	char addr[256], port[6];
	const char *spec;
	int fd, opt, domain;

	spec = path + strlen("tcp:");
	if (*spec == '\0') {
		warnx("USB serial tcp backend requires an address");
		return (-1);
	}

	if (sscanf(spec, "[%255[^]]]:%5s", addr, port) == 2) {
		domain = AF_INET6;
	} else if (sscanf(spec, ":%5s", port) == 1) {
		strcpy(addr, "127.0.0.1");
		domain = AF_INET;
	} else if (sscanf(spec, "%255[^:]:%5s", addr, port) == 2) {
		domain = AF_INET;
	} else {
		warnx("Invalid USB serial tcp backend '%s'", path);
		return (-1);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = domain;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;
	if (getaddrinfo(addr, port, &hints, &src_addr) != 0) {
		warnx("Invalid USB serial address %s:%s", addr, port);
		return (-1);
	}

	fd = socket(domain, SOCK_STREAM, 0);
	if (fd < 0) {
		freeaddrinfo(src_addr);
		return (-1);
	}

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0 ||
	    bind(fd, src_addr->ai_addr, src_addr->ai_addrlen) < 0 ||
	    fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || listen(fd, 1) < 0) {
		warn("USB serial tcp backend %s:%s", addr, port);
		close(fd);
		freeaddrinfo(src_addr);
		return (-1);
	}
	freeaddrinfo(src_addr);

	sc->backend.listen_fd = fd;
	sc->backend.listen_mev = mevent_add(fd, EVF_READ, userial_accept, sc);
	if (sc->backend.listen_mev == NULL) {
		close(fd);
		sc->backend.listen_fd = -1;
		return (-1);
	}
	return (0);
}

static int
userial_open_file(struct userial_softc *sc, const char *path)
{
	int fd;

	fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		warn("USB serial backend %s", path);
		return (-1);
	}

	sc->backend.fd = fd;
	sc->backend.read_mev = mevent_add(fd, EVF_READ, userial_readable, sc);
	if (sc->backend.read_mev == NULL) {
		close(fd);
		sc->backend.fd = -1;
		return (-1);
	}
	return (0);
}

static int
userial_open_backend(struct userial_softc *sc, const char *path)
{
	signal(SIGPIPE, SIG_IGN);

	sc->backend.fd = -1;
	sc->backend.listen_fd = -1;

	if (path == NULL || *path == '\0')
		return (0);
	if (strncmp(path, "unix:", 5) == 0)
		return (userial_open_unix(sc, path));
	if (strncmp(path, "tcp:", 4) == 0)
		return (userial_open_tcp(sc, path));
	return (userial_open_file(sc, path));
}

static void *
userial_init(struct usb_hci *hci, nvlist_t *nvl)
{
	struct userial_softc *sc;
	const char *path;

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		return (NULL);

	sc->hci = hci;
	pthread_mutex_init(&sc->mtx, NULL);
	USETDW(sc->line_state.dwDTERate, 115200);
	sc->line_state.bCharFormat = UCDC_STOP_BIT_1;
	sc->line_state.bParityType = UCDC_PARITY_NONE;
	sc->line_state.bDataBits = 8;

	path = get_config_value_node(nvl, "path");
	if (userial_open_backend(sc, path) != 0) {
		pthread_mutex_destroy(&sc->mtx);
		free(sc);
		return (NULL);
	}

	return (sc);
}

#define UREQ(x, y) ((x) | ((y) << 8))

static void
userial_get_data_block(struct usb_data_xfer *xfer,
    struct usb_data_xfer_block **data, uint8_t **udata)
{
	int i, idx;

	*data = NULL;
	*udata = NULL;
	idx = xfer->head;
	for (i = 0; i < xfer->ndata; i++) {
		xfer->data[idx].bdone = 0;
		if (*data == NULL && USB_DATA_OK(xfer, idx)) {
			*data = &xfer->data[idx];
			*udata = (*data)->buf;
		}
		xfer->data[idx].processed = 1;
		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
	}
}

static int
userial_copy_string_desc(struct usb_data_xfer_block *data, uint8_t *udata,
    uint16_t len, uint8_t index)
{
	const char *str;
	uint16_t slen;
	int i;

	if (index >= USERIAL_STR_MAX)
		return (USB_ERR_STALLED);

	str = userial_desc_strings[index];
	if (index == USERIAL_STR_LANG) {
		if (len > 4)
			len = 4;
		if (len > 0)
			udata[0] = 4;
		if (len > 1)
			udata[1] = UDESC_STRING;
		if (len > 2)
			udata[2] = str[0];
		if (len > 3)
			udata[3] = str[1];
		data->bdone += len;
		data->blen = 0;
		return (USB_ERR_NORMAL_COMPLETION);
	}

	slen = 2 + strlen(str) * 2;
	if (len > slen) {
		data->blen = len - slen;
		len = slen;
	} else {
		data->blen = 0;
	}
	if (len > 0)
		udata[0] = slen;
	if (len > 1)
		udata[1] = UDESC_STRING;
	for (i = 2; i + 1 < len; i += 2) {
		udata[i] = *str++;
		udata[i + 1] = '\0';
	}
	data->bdone += len;
	return (data->blen > 0 ? USB_ERR_SHORT_XFER : USB_ERR_NORMAL_COMPLETION);
}

static int
userial_request(void *scarg, struct usb_data_xfer *xfer)
{
	struct userial_softc *sc = scarg;
	struct usb_data_xfer_block *data;
	uint8_t *udata;
	uint16_t value, index, len;
	int err;

	userial_get_data_block(xfer, &data, &udata);
	err = USB_ERR_NORMAL_COMPLETION;
	if (xfer->ureq == NULL)
		return (err);

	value = UGETW(xfer->ureq->wValue);
	index = UGETW(xfer->ureq->wIndex);
	len = UGETW(xfer->ureq->wLength);

	DPRINTF(("userial request type 0x%x req 0x%x value 0x%x index 0x%x "
		 "len %u",
	    xfer->ureq->bmRequestType, xfer->ureq->bRequest, value, index,
	    len));

	pthread_mutex_lock(&sc->mtx);
	switch (UREQ(xfer->ureq->bRequest, xfer->ureq->bmRequestType)) {
	case UREQ(UR_GET_CONFIG, UT_READ_DEVICE):
		if (data != NULL && len > 0) {
			*udata = sc->configured ?
			    userial_confd.confd.bConfigurationValue : 0;
			data->bdone += 1;
			data->blen = len - 1;
			if (data->blen > 0)
				err = USB_ERR_SHORT_XFER;
		}
		break;

	case UREQ(UR_GET_DESCRIPTOR, UT_READ_DEVICE):
		if (data == NULL)
			break;
		switch (value >> 8) {
		case UDESC_DEVICE:
			if ((value & 0xff) != 0) {
				err = USB_ERR_STALLED;
				break;
			}
			if (len > sizeof(userial_dev_desc)) {
				data->blen = len - sizeof(userial_dev_desc);
				len = sizeof(userial_dev_desc);
			} else {
				data->blen = 0;
			}
			memcpy(data->buf, &userial_dev_desc, len);
			data->bdone += len;
			if (data->blen > 0)
				err = USB_ERR_SHORT_XFER;
			break;
		case UDESC_CONFIG:
			if ((value & 0xff) != 0) {
				err = USB_ERR_STALLED;
				break;
			}
			if (len > sizeof(userial_confd)) {
				data->blen = len - sizeof(userial_confd);
				len = sizeof(userial_confd);
			} else {
				data->blen = 0;
			}
			memcpy(data->buf, &userial_confd, len);
			data->bdone += len;
			if (data->blen > 0)
				err = USB_ERR_SHORT_XFER;
			break;
		case UDESC_STRING:
			err = userial_copy_string_desc(data, udata, len,
			    value & 0xff);
			break;
		default:
			err = USB_ERR_STALLED;
			break;
		}
		break;

	case UREQ(UR_GET_INTERFACE, UT_READ_INTERFACE):
		if (index > 1) {
			err = USB_ERR_STALLED;
			break;
		}
		if (data != NULL && len > 0) {
			*udata = 0;
			data->bdone += 1;
			data->blen = len - 1;
			if (data->blen > 0)
				err = USB_ERR_SHORT_XFER;
		}
		break;

	case UREQ(UR_GET_STATUS, UT_READ_DEVICE):
	case UREQ(UR_GET_STATUS, UT_READ_INTERFACE):
	case UREQ(UR_GET_STATUS, UT_READ_ENDPOINT):
		if (data != NULL && len >= 2) {
			USETW(udata, 0);
			data->bdone += 2;
			data->blen = len - 2;
			if (data->blen > 0)
				err = USB_ERR_SHORT_XFER;
		}
		break;

	case UREQ(UR_SET_ADDRESS, UT_WRITE_DEVICE):
		break;
	case UREQ(UR_SET_CONFIG, UT_WRITE_DEVICE):
		sc->configured = (value != USB_UNCONFIG_NO);
		sc->notify_pending = sc->configured;
		break;
	case UREQ(UR_SET_INTERFACE, UT_WRITE_INTERFACE):
		if (index > 1 || value != 0)
			err = USB_ERR_STALLED;
		break;
	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_DEVICE):
	case UREQ(UR_SET_FEATURE, UT_WRITE_DEVICE):
	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_INTERFACE):
	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_ENDPOINT):
	case UREQ(UR_SET_FEATURE, UT_WRITE_INTERFACE):
	case UREQ(UR_SET_FEATURE, UT_WRITE_ENDPOINT):
	case UREQ(UR_ISOCH_DELAY, UT_WRITE_DEVICE):
	case UREQ(UR_SET_SEL, 0):
	case UREQ(UR_SYNCH_FRAME, UT_WRITE_ENDPOINT):
		break;

	case UREQ(UCDC_GET_LINE_CODING, UT_READ_CLASS_INTERFACE):
		if (data == NULL)
			break;
		if (index != 0 || len < UCDC_LINE_STATE_LENGTH) {
			err = USB_ERR_STALLED;
			break;
		}
		memcpy(data->buf, &sc->line_state, UCDC_LINE_STATE_LENGTH);
		data->bdone += UCDC_LINE_STATE_LENGTH;
		data->blen = len - UCDC_LINE_STATE_LENGTH;
		if (data->blen > 0)
			err = USB_ERR_SHORT_XFER;
		break;

	case UREQ(UCDC_SET_LINE_CODING, UT_WRITE_CLASS_INTERFACE):
		if (index != 0 || data == NULL || len < UCDC_LINE_STATE_LENGTH) {
			err = USB_ERR_STALLED;
			break;
		}
		memcpy(&sc->line_state, data->buf, UCDC_LINE_STATE_LENGTH);
		data->bdone += UCDC_LINE_STATE_LENGTH;
		data->blen = 0;
		break;

	case UREQ(UCDC_SET_CONTROL_LINE_STATE, UT_WRITE_CLASS_INTERFACE):
		if (index != 0) {
			err = USB_ERR_STALLED;
			break;
		}
		sc->control_line_state = value;
		sc->notify_pending = true;
		break;

	case UREQ(UCDC_SEND_BREAK, UT_WRITE_CLASS_INTERFACE):
		if (index != 0)
			err = USB_ERR_STALLED;
		break;

	default:
		err = USB_ERR_STALLED;
		break;
	}
	pthread_mutex_unlock(&sc->mtx);

	if (xfer->ureq != NULL && (xfer->ureq->bmRequestType & UT_WRITE) &&
	    err == USB_ERR_NORMAL_COMPLETION && data != NULL)
		data->blen = 0;

	return (err);
}

static int
userial_notify(struct userial_softc *sc, struct usb_data_xfer *xfer)
{
	struct usb_data_xfer_block *data;
	struct usb_cdc_notification notify;
	uint8_t *udata;
	uint16_t state;
	int len, idx;

	idx = xfer->head;
	data = &xfer->data[idx];
	if (data->buf == NULL || data->blen == 0) {
		data->processed = 1;
		return (USB_ERR_NORMAL_COMPLETION);
	}

	pthread_mutex_lock(&sc->mtx);
	if (!sc->notify_pending) {
		pthread_mutex_unlock(&sc->mtx);
		USB_DATA_SET_ERRCODE(data, USB_NAK);
		return (USB_ERR_CANCELLED);
	}
	sc->notify_pending = false;
	state = UCDC_N_SERIAL_DSR | UCDC_N_SERIAL_DCD;
	if (sc->control_line_state & UCDC_LINE_DTR)
		state |= UCDC_N_SERIAL_DSR;
	pthread_mutex_unlock(&sc->mtx);

	memset(&notify, 0, sizeof(notify));
	notify.bmRequestType = UCDC_NOTIFICATION;
	notify.bNotification = UCDC_N_SERIAL_STATE;
	USETW(notify.wIndex, 0);
	USETW(notify.wLength, 2);
	USETW(notify.data, state);

	udata = data->buf;
	len = MIN(data->blen, UCDC_NOTIFICATION_LENGTH + 2);
	memcpy(udata, &notify, len);
	data->bdone += len;
	data->blen -= len;
	data->processed = 1;
	return (data->blen > 0 ? USB_ERR_SHORT_XFER :
	    USB_ERR_NORMAL_COMPLETION);
}

static int
userial_backend_read(struct userial_softc *sc, void *buf, size_t len)
{
	ssize_t nread;

	pthread_mutex_lock(&sc->mtx);
	if (sc->backend.fd < 0) {
		pthread_mutex_unlock(&sc->mtx);
		return (-1);
	}
	nread = read(sc->backend.fd, buf, len);
	if (nread == 0) {
		userial_disconnect_locked(sc);
		pthread_mutex_unlock(&sc->mtx);
		return (-1);
	}
	if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		userial_disconnect_locked(sc);
		pthread_mutex_unlock(&sc->mtx);
		return (-1);
	}
	pthread_mutex_unlock(&sc->mtx);

	if (nread < 0)
		return (0);
	return ((int)nread);
}

static int
userial_backend_write(struct userial_softc *sc, const void *buf, size_t len)
{
	ssize_t nwritten;

	pthread_mutex_lock(&sc->mtx);
	if (sc->backend.fd < 0) {
		pthread_mutex_unlock(&sc->mtx);
		return ((int)len);
	}
	nwritten = write(sc->backend.fd, buf, len);
	if (nwritten < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		userial_disconnect_locked(sc);
		pthread_mutex_unlock(&sc->mtx);
		return (0);
	}
	pthread_mutex_unlock(&sc->mtx);

	if (nwritten < 0)
		return (0);
	return ((int)nwritten);
}

static int
userial_bulk_in(struct userial_softc *sc, struct usb_data_xfer *xfer)
{
	struct usb_data_xfer_block *data;
	int idx, i, nread;

	idx = xfer->head;
	for (i = 0; i < xfer->ndata; i++) {
		data = &xfer->data[idx];
		if (data->processed || data->buf == NULL || data->blen == 0) {
			data->processed = 1;
			idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
			continue;
		}

		nread = userial_backend_read(sc, data->buf, data->blen);
		if (nread <= 0) {
			USB_DATA_SET_ERRCODE(data, USB_NAK);
			return (USB_ERR_CANCELLED);
		}
		data->bdone += nread;
		data->blen -= nread;
		data->processed = 1;
		return (data->blen > 0 ? USB_ERR_SHORT_XFER :
		    USB_ERR_NORMAL_COMPLETION);
	}

	return (USB_ERR_NORMAL_COMPLETION);
}

static int
userial_bulk_out(struct userial_softc *sc, struct usb_data_xfer *xfer)
{
	struct usb_data_xfer_block *data;
	uint8_t *buf;
	int idx, i, nwritten;

	idx = xfer->head;
	for (i = 0; i < xfer->ndata; i++) {
		data = &xfer->data[idx];
		if (data->processed || data->buf == NULL || data->blen == 0) {
			data->processed = 1;
			idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
			continue;
		}

		buf = (uint8_t *)data->buf + data->bdone;
		nwritten = userial_backend_write(sc, buf, data->blen);
		data->bdone += nwritten;
		data->blen -= nwritten;
		if (data->blen == 0)
			data->processed = 1;
		idx = (idx + 1) % USB_MAX_XFER_BLOCKS;
	}

	return (USB_ERR_NORMAL_COMPLETION);
}

static int
userial_data(void *scarg, struct usb_data_xfer *xfer, int dir, int epctx)
{
	struct userial_softc *sc = scarg;

	if (epctx == USERIAL_NOTIFY_ENDPT)
		return (userial_notify(sc, xfer));
	if (epctx == USERIAL_BULK_IN_ENDPT && dir)
		return (userial_bulk_in(sc, xfer));
	if (epctx == USERIAL_BULK_OUT_ENDPT && !dir)
		return (userial_bulk_out(sc, xfer));

	return (USB_ERR_STALLED);
}

static int
userial_reset(void *scarg)
{
	struct userial_softc *sc = scarg;

	pthread_mutex_lock(&sc->mtx);
	sc->configured = false;
	sc->notify_pending = false;
	pthread_mutex_unlock(&sc->mtx);
	return (0);
}

static int
userial_remove(void *scarg)
{
	struct userial_softc *sc = scarg;

	if (sc == NULL)
		return (0);
	pthread_mutex_lock(&sc->mtx);
	userial_disconnect_locked(sc);
	if (sc->backend.listen_mev != NULL) {
		mevent_delete(sc->backend.listen_mev);
		sc->backend.listen_mev = NULL;
	}
	if (sc->backend.listen_fd >= 0) {
		close(sc->backend.listen_fd);
		sc->backend.listen_fd = -1;
	}
	if (sc->backend.unix_path != NULL) {
		(void)unlink(sc->backend.unix_path);
		free(sc->backend.unix_path);
		sc->backend.unix_path = NULL;
	}
	pthread_mutex_unlock(&sc->mtx);
	pthread_mutex_destroy(&sc->mtx);
	free(sc);
	return (0);
}

static int
userial_stop(void *scarg)
{
	struct userial_softc *sc = scarg;

	pthread_mutex_lock(&sc->mtx);
	sc->configured = false;
	sc->notify_pending = false;
	pthread_mutex_unlock(&sc->mtx);
	return (0);
}

static struct usb_devemu ue_serial = {
	.ue_emu = "serial",
	.ue_usbver = 2,
	.ue_usbspeed = USB_SPEED_HIGH,
	.ue_init = userial_init,
	.ue_request = userial_request,
	.ue_data = userial_data,
	.ue_reset = userial_reset,
	.ue_remove = userial_remove,
	.ue_stop = userial_stop,
};
USB_EMUL_SET(ue_serial);
