/* SPDX-License-Identifier: BSD-2-Clause */

/* Exercise the real device/host-controller path with guest RAM and RX mocked. */
#define netbe_recv test_netbe_recv
#include "../src/usb_net.c"
#undef netbe_recv
#undef DPRINTF
#undef WPRINTF
#define paddr_guest2host test_guest2host
#include "../src/pci_xhci.c"
#undef paddr_guest2host

static uint8_t guest_ram[65536] __aligned(4096);
static unsigned queued_packets, packet_size;

void *
test_guest2host(struct vmctx *ctx __unused, uintptr_t addr, size_t len)
{

	assert(addr <= sizeof(guest_ram) && len <= sizeof(guest_ram) - addr);
	return (guest_ram + addr);
}

ssize_t
test_netbe_recv(net_backend_t *be __unused, const struct iovec *iov, int niov)
{
	uint8_t packet[UNET_MTU_SIZE];

	if (queued_packets == 0)
		return (0);
	assert(packet_size <= sizeof(packet));
	memset(packet, 0x5a, packet_size);
	queued_packets--;
	assert(buf_to_iov(packet, packet_size, iov, niov, 0) == packet_size);
	return (packet_size);
}

static void
receive_test(bool split, bool nak, bool isp, unsigned head, unsigned packets,
    unsigned size)
{
	struct pci_devinst pi = {0};
	struct pci_xhci_softc hc = {.xsc_pi = &pi};
	struct unet_softc net = {.ntb_input_size = 8192};
	struct pci_xhci_dev_emu dev = {.xsc = &hc, .dev_ue = &ue_net,
	    .dev_sc = &net, .dev_slotstate = XHCI_ST_CONFIGURED};
	struct pci_xhci_dev_emu *slots[XHCI_MAX_SLOTS] = {&dev};
	struct xhci_dcbaa dcbaa = {0};
	struct xhci_event_ring_seg segment = {.qwEvrsTablePtr = 0xe000,
	    .dwEvrsTableSize = 16};
	struct usb_data_xfer xfer = {.head = head, .tail = head};
	struct pci_xhci_dev_ep *ep = &dev.eps[7];
	struct xhci_dev_ctx *ctx;
	struct xhci_trb *trbs, *event;
	struct usb_ncm16_hdr *ntb;
	struct usb_ncm16_dpt *ndp;
	struct usb_ncm16_dp *dp;
	unsigned ndata = split ? 2 : 1;
	unsigned total = sizeof(struct usb_ncm16_hdr) +
	    sizeof(struct usb_ncm16_dpt) +
	    (UNET_NTB_MAX_IN_DGRAMS + 1) * sizeof(struct usb_ncm16_dp) +
	    packets * size;

	memset(guest_ram, 0, sizeof(guest_ram));
	memset(guest_ram + 0x4000, 0xcc, 8192);
	memset(guest_ram + 0x7000, 0xcc, 4096);
	ctx = (void *)(guest_ram + 0x1000);
	trbs = (void *)(guest_ram + 0x2000);
	event = (void *)(guest_ram + 0xe000);
	dcbaa.dcba[1] = 0x1000;
	hc.slots = slots;
	hc.opregs.dcbaa_p = &dcbaa;
	hc.rtsregs.erstba_p = &segment;
	hc.rtsregs.erst_p = event;
	hc.rtsregs.intrreg.erdp = 0xe000;
	hc.rtsregs.event_pcs = 1;
	dev.dev_ctx = ctx;
	ep->ep_xfer = &xfer;
	ep->ep_tr = trbs;
	ep->ep_ringaddr = 0x2000;
	ep->ep_ccs = 1;
	ctx->ctx_ep[7].qwEpCtx2 = 0x2001;
	for (unsigned i = 0; i < ndata; i++) {
		trbs[i].qwTrb0 = i == 0 ? 0x4000 : 0x7000;
		trbs[i].dwTrb2 = split ? 4096 : 8192;
		trbs[i].dwTrb3 = XHCI_TRB_3_TYPE_SET(XHCI_TRB_TYPE_NORMAL) |
		    XHCI_TRB_3_CYCLE_BIT | XHCI_TRB_3_CHAIN_BIT;
	}
	/* Match Windows' ENT + EVENT_DATA, and also exercise an ISP event. */
	trbs[ndata - 1].dwTrb3 |= XHCI_TRB_3_ENT_BIT;
	if (isp)
		trbs[ndata - 1].dwTrb3 |= XHCI_TRB_3_ISP_BIT;
	trbs[ndata].qwTrb0 = 0x12345678;
	trbs[ndata].dwTrb3 = XHCI_TRB_3_TYPE_SET(XHCI_TRB_TYPE_EVENT_DATA) |
	    XHCI_TRB_3_CYCLE_BIT | XHCI_TRB_3_IOC_BIT;

	queued_packets = nak ? 0 : packets;
	packet_size = size;
	if (nak) {
		/* Repeated empty retries must retain every buffer and emit no event. */
		for (unsigned i = 0; i < 3; i++) {
			pci_xhci_device_doorbell(&hc, 1, 7, 0);
			assert(hc.rtsregs.er_events_cnt == 0);
			assert(xfer.ndata == ndata + 1);
			assert(ep->ep_ringaddr == 0x2000);
		}
		/* Error status alone must not let completion retire a data TRB. */
		int intr;
		assert(pci_xhci_xfer_complete(&hc, &xfer, 1, 7,
		    XHCI_TRB_ERROR_SUCCESS, &intr) == XHCI_TRB_ERROR_SUCCESS);
		assert(intr == 0 && xfer.ndata == ndata + 1);
		assert(ep->ep_ringaddr == 0x2000);
		queued_packets = packets;
	}
	pci_xhci_device_doorbell(&hc, 1, 7, 0);
	assert(queued_packets == 0);
	assert(xfer.ndata == 0);
	assert(ep->ep_ringaddr == 0x2000 + (ndata + 1) * sizeof(*trbs));
	assert(hc.rtsregs.er_events_cnt == (isp ? 2 : 1));
	event += hc.rtsregs.er_events_cnt - 1;
	assert(event->qwTrb0 == 0x12345678);
	assert(event->dwTrb3 & XHCI_TRB_3_ED_BIT);
	assert((event->dwTrb2 & 0xffffff) == total);
	assert(XHCI_TRB_2_ERROR_GET(event->dwTrb2) == XHCI_TRB_ERROR_SHORT_PKT);
	ntb = (void *)(guest_ram + 0x4000);
	assert(memcmp(ntb->dwSignature, "NCMH", 4) == 0);
	assert(UGETW(ntb->wBlockLength) == total);
	ndp = (void *)((uint8_t *)ntb + UGETW(ntb->wDptIndex));
	dp = (void *)(ndp + 1);
	for (unsigned i = 0; i < packets; i++) {
		assert(UGETW(dp[i].wFrameLength) == size);
		for (unsigned j = 0; j < size; j++) {
			unsigned offset = UGETW(dp[i].wFrameIndex) + j;
			unsigned addr = split && offset >= 4096 ?
			    0x7000 + offset - 4096 : 0x4000 + offset;
			assert(guest_ram[addr] == 0x5a);
		}
	}
	assert(UGETW(dp[packets].wFrameIndex) == 0);
	assert(UGETW(dp[packets].wFrameLength) == 0);
}

int
main(int argc, char **argv)
{
	bool nak = argc > 1 && strcmp(argv[1], "retry") == 0;

	for (unsigned isp = 0; isp < 2; isp++)
		for (unsigned head = 0; head < USB_MAX_XFER_BLOCKS; head++) {
			receive_test(false, nak, isp, head, 1, 100);
			receive_test(true, nak, isp, head, 1, 100);
			/* End at the first TRB boundary, or span both data TRBs. */
			receive_test(true, nak, isp, head, 4, 1010);
			receive_test(true, nak, isp, head, 5, 1000);
		}
	return (0);
}
