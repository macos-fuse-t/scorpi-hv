/* SPDX-License-Identifier: BSD-2-Clause */
#include "usb_emul.h"
#undef USB_EMUL_SET
#define USB_EMUL_SET(x)
#include "../src/usb_kbd.c"

#include <assert.h>

static int
keyboard_interrupt(struct usb_hci *hci __unused, int endpoint)
{

	assert(endpoint == (UE_DIR_IN | UKBD_INTR_ENDPT));
	return (0);
}

static struct ukbd_report
read_report(struct ukbd_softc *sc)
{
	struct ukbd_report report = {0};
	struct usb_data_xfer xfer = {.ndata = 1};

	xfer.data[0].buf = &report;
	xfer.data[0].blen = sizeof(report);
	assert(ukbd_data_handler(sc, &xfer, USB_XFER_IN, 1) ==
	    USB_ERR_NORMAL_COMPLETION);
	assert(xfer.data[0].bdone == sizeof(report));
	assert(!sc->newdata);
	return (report);
}

static unsigned
count_key(struct ukbd_report report, uint8_t key)
{
	unsigned count = 0;

	for (unsigned i = 0; i < sizeof(report.keys); i++)
		count += report.keys[i] == key;
	return (count);
}

int
main(void)
{
	struct usb_hci hci = {.hci_intr = keyboard_interrupt};
	struct ukbd_softc sc = {.hci = &hci};
	struct ukbd_report report;
	const uint8_t keys[] = {7, 26, 4}; /* D, W, A */

	pthread_mutex_init(&sc.mtx, NULL);
	/* Release an earlier key, then receive host autorepeat for later keys. */
	ukbd_event(1, 5, 0, &sc);
	for (unsigned i = 0; i < sizeof(keys); i++)
		ukbd_event(1, keys[i], 0, &sc);
	ukbd_event(0, 5, 0, &sc);
	for (unsigned i = 0; i < sizeof(keys); i++) {
		for (unsigned repeat = 0; repeat < 10; repeat++) {
			ukbd_event(1, keys[i], 0, &sc);
			report = read_report(&sc);
			assert(count_key(report, keys[i]) == 1);
		}
		ukbd_event(0, keys[i], 0, &sc);
		report = read_report(&sc);
		assert(count_key(report, keys[i]) == 0);
		for (unsigned j = i + 1; j < sizeof(keys); j++)
			assert(count_key(report, keys[j]) == 1);
	}

	/* One release also recovers reports containing legacy duplicates. */
	sc.um_report.keys[0] = sc.um_report.keys[2] = 4;
	sc.um_report.keys[1] = 26;
	ukbd_event(0, 4, 0, &sc);
	report = read_report(&sc);
	assert(count_key(report, 4) == 0 && count_key(report, 26) == 1);
	ukbd_event(0, 26, 0, &sc);

	/* Preserve six-key rollover, repeated downs, and modifier-only events. */
	for (unsigned key = 4; key < 10; key++)
		ukbd_event(1, key, 0, &sc);
	ukbd_event(1, 10, 0, &sc);
	ukbd_event(0, 0, 2, &sc);
	report = read_report(&sc);
	assert(report.modifiers == 2 && count_key(report, 10) == 0);
	for (unsigned key = 4; key < 10; key++)
		assert(count_key(report, key) == 1);
	for (unsigned key = 4; key < 10; key++)
		ukbd_event(0, key, 0, &sc);
	report = read_report(&sc);
	assert(report.modifiers == 0 && count_key(report, 0) == 6);
	pthread_mutex_destroy(&sc.mtx);
	puts("USB keyboard repeat, release, and rollover checks passed");
	return (0);
}
