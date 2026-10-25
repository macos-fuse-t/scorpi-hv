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
	uint8_t packet[2] = { value & 0xff, *feature };
	xfer.data[0].buf = packet;
	xfer.data[0].blen = length;
	int err = umouse_request(sc, &xfer);
	if (!write && !err) {
		assert(packet[0] == (value & 0xff));
		*feature = packet[1];
	}
	return (err);
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

/* Validate the binary descriptor independently of the report writer. */
static void
check_descriptor(void)
{
	unsigned id = 0, size = 0, count = 0, depth = 0, applications = 0;
	unsigned input_bits[3] = { 0 }, feature_bits[3] = { 0 };
	int logical_minimum = 0;
	bool axes[3] = { false };
	for (size_t i = 0; i < sizeof(umouse_report_desc);) {
		uint8_t key = umouse_report_desc[i++];
		unsigned bytes = key & 3;
		if (bytes == 3)
			bytes = 4;
		assert(i + bytes <= sizeof(umouse_report_desc));
		uint32_t value = 0;
		for (unsigned n = 0; n < bytes; n++)
			value |= (uint32_t)umouse_report_desc[i++] << (8 * n);
		switch (key & 0xfc) {
		case 0x14:
			logical_minimum = bytes == 1 ? (int8_t)value :
						       (int16_t)value;
			break;
		case 0x84:
			id = value;
			assert(id == 1 || id == 2);
			break;
		case 0x74:
			size = value;
			break;
		case 0x94:
			count = value;
			break;
		case 0xa0:
			if (!depth++) {
				assert(value == 1);
				applications++;
			}
			break;
		case 0xc0:
			assert(depth);
			depth--;
			break;
		case 0x80:
			assert(id && id < 3);
			input_bits[id] += size * count;
			if (size == 16 && count == 2) {
				assert((value & 4) == (id == 2 ? 4 : 0));
				assert(
				    logical_minimum == (id == 2 ? -32767 : 0));
				axes[id] = true;
			}
			break;
		case 0xb0:
			feature_bits[id] += size * count;
			break;
		}
	}
	assert(depth == 0 && applications == 2 && axes[1] && axes[2]);
	assert(input_bits[1] == 48 && input_bits[2] == 48);
	assert(feature_bits[1] == 8 && feature_bits[2] == 8);
	assert(
	    UGETW(umouse_confd.hidd.wItemLength) == sizeof(umouse_report_desc));
}

int
main(void)
{
	struct usb_hci hci = {.hci_intr = mouse_interrupt};
	struct umouse_softc sc = {.hci = &hci};
	uint8_t feature = 99;
	struct umouse_report report;

	pthread_mutex_init(&sc.mtx, NULL);
	check_descriptor();
	assert(sizeof(report) == 7);
	assert(umouse_confd.endpd.bInterval == 4);
	assert(feature_request(&sc, false, &feature, 0x0301, 2) == 0);
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
	assert(feature_request(&sc, true, &feature, 0x0301, 2) == 0);
	feature = 0;
	assert(feature_request(&sc, false, &feature, 0x0301, 2) == 0);
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
	assert(
	    feature_request(&sc, true, &feature, 0x0301, 2) == USB_ERR_STALLED);
	assert(
	    feature_request(&sc, true, &feature, 0x0303, 2) == USB_ERR_STALLED);
	assert(
	    feature_request(&sc, true, &feature, 0x0301, 0) == USB_ERR_STALLED);
	assert(sc.hid.feature == 1);

	/* An undersized packet must neither overflow nor consume pending motion. */
	umouse_scroll_event(0, 10, 20, 6, &sc);
	uint8_t guard[7];
	memset(guard, 0xA5, sizeof(guard));
	struct usb_data_xfer short_xfer = {.ndata = 1};
	short_xfer.data[0].buf = guard;
	short_xfer.data[0].blen = 6;
	assert(umouse_data_handler(&sc, &short_xfer, USB_XFER_IN, 1) == USB_ERR_STALLED);
	for (unsigned i = 0; i < sizeof(guard); i++)
		assert(guard[i] == 0xA5);
	assert(read_report(&sc).z == 6);

	umouse_scroll_event(0, 10, 20, 7, &sc);
	assert(umouse_reset(&sc) == 0);
	assert(sc.pending_scroll == 0 && sc.hid.feature == 0);
	assert_no_report(&sc);

	/* Relative reports have independent signed axes and wheel negotiation.
	 */
	umouse_relative_event(1, 100000, -100000, 120, &sc);
	umouse_relative_event(0, 0, 0, 0, &sc); /* preserve the button-up */
	int64_t total_x = 0, total_y = 0;
	for (int i = 0; i < 4; i++) {
		report = read_report(&sc);
		assert(report.report_id == UMOUSE_RELATIVE_REPORT &&
		    report.buttons == 1);
		total_x += report.x;
		total_y += report.y;
		assert(report.z == (i == 0 ? 1 : 0));
	}
	assert(total_x == 100000 && total_y == -100000);
	report = read_report(&sc);
	assert(
	    report.report_id == UMOUSE_RELATIVE_REPORT && report.buttons == 0);
	assert(report.x == 0 && report.y == 0);
	assert_no_report(&sc);
	/* Continued motion does not depend on the absolute tablet position. */
	for (int i = 0; i < 100; i++) {
		umouse_relative_event(0, -10, 0, 0, &sc);
		report = read_report(&sc);
		assert(
		    report.report_id == 2 && report.x == -10 && report.y == 0);
	}
	umouse_relative_event(0, 0, 0, 30, &sc);
	assert(read_report(&sc).z == 0);
	assert_no_report(&sc);
	umouse_relative_event(0, 0, 0, 90, &sc);
	assert(read_report(&sc).z == 1);
	feature = 1;
	assert(feature_request(&sc, true, &feature, 0x0302, 2) == 0);
	assert(sc.relative_feature == 1 && sc.hid.feature == 0);
	umouse_relative_event(2, 0, 0, -250, &sc);
	assert(read_report(&sc).z == -127);
	assert(read_report(&sc).z == -123);
	assert_no_report(&sc);
	umouse_event(0, 20, 30, &sc);
	report = read_report(&sc);
	assert(
	    report.report_id == UMOUSE_RELATIVE_REPORT && report.buttons == 0);
	assert(read_report(&sc).report_id == UMOUSE_ABSOLUTE_REPORT);
	umouse_event(1, 20, 30, &sc);
	assert(read_report(&sc).buttons == 1);
	umouse_relative_event(0, 10, -10, 0, &sc);
	report = read_report(&sc);
	assert(
	    report.report_id == UMOUSE_ABSOLUTE_REPORT && report.buttons == 0);
	report = read_report(&sc);
	assert(report.report_id == UMOUSE_RELATIVE_REPORT && report.x == 10);
	assert_no_report(
	    &sc); /* No stale absolute coordinates after switching. */
	umouse_relative_event(1, 200, 300, 400, &sc);
	umouse_reset(&sc);
	assert_no_report(&sc);
	assert(sc.relative_buttons == 0 && sc.relative_feature == 0);
	pthread_mutex_destroy(&sc.mtx);
	puts("USB absolute/relative HID, continuous motion, button ordering, "
	     "wheel and packet checks passed");
	return (0);
}
