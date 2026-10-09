/* SPDX-License-Identifier: BSD-2-Clause */
#include "usb_emul.h"
#undef USB_EMUL_SET
#define USB_EMUL_SET(x)
#include "../src/usb_mouse.c"

#include <assert.h>

static int
mouse_interrupt(struct usb_hci *hci __unused, int endpoint)
{
	assert(endpoint == (UE_DIR_IN | UMOUSE_INTR_ENDPT));
	return (0);
}

static struct umouse_report
read_report(struct umouse_softc *sc)
{
	struct umouse_report report = {0};
	struct usb_data_xfer xfer = {.ndata = 1};

	xfer.data[0].buf = &report;
	xfer.data[0].blen = sizeof(report);
	assert(umouse_data_handler(sc, &xfer, USB_XFER_IN, 1) ==
	    USB_ERR_NORMAL_COMPLETION);
	assert(xfer.data[0].bdone == sizeof(report));
	return (report);
}

static int
feature_request(struct umouse_softc *sc, bool write, uint8_t *feature,
    uint16_t value, uint16_t length)
{
	struct usb_device_request request = {
		.bmRequestType = write ? UT_WRITE_CLASS_INTERFACE : UT_READ_CLASS_INTERFACE,
		.bRequest = write ? UMOUSE_SET_REPORT : UMOUSE_GET_REPORT,
	};
	struct usb_data_xfer xfer = {.ndata = 1, .ureq = &request};

	USETW(request.wValue, value);
	USETW(request.wLength, length);
	xfer.data[0].buf = feature;
	xfer.data[0].blen = length;
	return (umouse_request(sc, &xfer));
}

static void
assert_no_report(struct umouse_softc *sc)
{
	struct umouse_report report;
	struct usb_data_xfer xfer = {.ndata = 1};

	xfer.data[0].buf = &report;
	xfer.data[0].blen = sizeof(report);
	assert(umouse_data_handler(sc, &xfer, USB_XFER_IN, 1) == USB_ERR_CANCELLED);
	assert(USB_DATA_GET_ERRCODE(&xfer.data[0]) == USB_NAK);
}

int
main(void)
{
	struct usb_hci hci = {.hci_intr = mouse_interrupt};
	struct umouse_softc sc = {.hci = &hci};
	uint8_t feature = 99;
	struct umouse_report report;

	pthread_mutex_init(&sc.mtx, NULL);
	assert(sizeof(report) == 6);
	assert(umouse_confd.endpd.bInterval == 4);
	assert(feature_request(&sc, false, &feature, 0x0300, 1) == 0);
	assert(feature == 0); /* Guests default to one count per detent. */

	/* Legacy viewer wheel events survive a following pointer movement. */
	umouse_event(8 | 1, 10, 20, &sc);
	umouse_event(1, 11, 21, &sc);
	report = read_report(&sc);
	assert(report.z == 1 && report.buttons == 1);
	assert_no_report(&sc);
	umouse_event(16, 11, 21, &sc);
	assert(read_report(&sc).z == -1);

	/* Small precise deltas accumulate for guests without high-res support. */
	umouse_scroll_event(0, 10, 20, 30, &sc);
	assert(read_report(&sc).z == 0);
	assert_no_report(&sc);
	umouse_scroll_event(0, 10, 20, 90, &sc);
	assert(read_report(&sc).z == 1);
	assert(sc.pending_scroll == 0);
	umouse_scroll_event(0, 10, 20, -119, &sc);
	assert(read_report(&sc).z == 0);
	umouse_scroll_event(0, 10, 20, -1, &sc);
	assert(read_report(&sc).z == -1);

	/* Negotiate a 120x multiplier through the real HID control handler. */
	feature = 1;
	assert(feature_request(&sc, true, &feature, 0x0300, 1) == 0);
	feature = 0;
	assert(feature_request(&sc, false, &feature, 0x0300, 1) == 0);
	assert(feature == 1);
	umouse_scroll_event(2, 10, 20, 3, &sc);
	report = read_report(&sc);
	assert(report.z == 3 && report.buttons == 2);
	umouse_event(8, 10, 20, &sc);
	assert(read_report(&sc).z == 120); /* A mouse notch is still a full notch. */

	/* Multiple host events and capped packets preserve their total distance. */
	umouse_scroll_event(0, 10, 20, 250, &sc);
	umouse_scroll_event(0, 10, 20, 50, &sc);
	umouse_event(0, 12, 22, &sc);
	assert(read_report(&sc).z == 127);
	assert(read_report(&sc).z == 127);
	assert(read_report(&sc).z == 46);
	assert_no_report(&sc);
	umouse_scroll_event(0, 10, 20, -300, &sc);
	assert(read_report(&sc).z == -127);
	assert(read_report(&sc).z == -127);
	assert(read_report(&sc).z == -46);

	/* Bad feature writes must leave the negotiated mode intact. */
	feature = 2;
	assert(feature_request(&sc, true, &feature, 0x0300, 1) == USB_ERR_STALLED);
	assert(feature_request(&sc, true, &feature, 0x0301, 1) == USB_ERR_STALLED);
	assert(feature_request(&sc, true, &feature, 0x0300, 0) == USB_ERR_STALLED);
	assert(sc.hid.feature == 1);

	/* An undersized packet must neither overflow nor consume pending motion. */
	umouse_scroll_event(0, 10, 20, 6, &sc);
	uint8_t guard[6];
	memset(guard, 0xA5, sizeof(guard));
	struct usb_data_xfer short_xfer = {.ndata = 1};
	short_xfer.data[0].buf = guard;
	short_xfer.data[0].blen = 5;
	assert(umouse_data_handler(&sc, &short_xfer, USB_XFER_IN, 1) == USB_ERR_STALLED);
	for (unsigned i = 0; i < sizeof(guard); i++)
		assert(guard[i] == 0xA5);
	assert(read_report(&sc).z == 6);

	umouse_scroll_event(0, 10, 20, 7, &sc);
	assert(umouse_reset(&sc) == 0);
	assert(sc.pending_scroll == 0 && sc.hid.feature == 0);
	assert_no_report(&sc);
	pthread_mutex_destroy(&sc.mtx);
	puts("USB wheel negotiation, accumulation, legacy fallback, and packet checks passed");
	return (0);
}
