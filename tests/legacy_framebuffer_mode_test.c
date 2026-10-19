/* SPDX-License-Identifier: BSD-2-Clause */
#include "pci_emul.h"
#undef PCI_EMUL_SET
#define PCI_EMUL_SET(x)
#include "../src/pci_virtio_gpu.c"

int raw_stdio;
static unsigned int notifications;
static int display_width, display_height, display_stride;

void
console_set_scanout(bool active, int w, int h, int stride, uint32_t format,
    const char *name, size_t size, bool redraw)
{
	assert(active && redraw);
	assert(format == VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
	assert(strcmp(name, "/legacy-mode-test") == 0);
	assert(size == FB_SIZE);
	display_width = w;
	display_height = h;
	display_stride = stride;
	notifications++;
}

int
main(void)
{
	struct pci_devinst pi = {0};
	struct pci_vgpu_softc sc = {0};
	char backing[] = {0x5a};
	struct vgpu_scanout scanout = {
		.resource_id = VGPU_LEGACY_RESOURCE_ID,
		.base_ptr = backing,
		.size = FB_SIZE,
	};

	pi.pi_arg = &sc;
	sc.resx = 800;
	sc.resy = 600;
	LIST_INIT(&sc.scanouts);

	/* Mode writes before the framebuffer exists must be harmless. */
	pci_vgpu_legacy_ctrl_write(&pi, 0, 4, 2, 800);
	assert(notifications == 0);

	strcpy(scanout.shm_name, "/legacy-mode-test");
	LIST_INSERT_HEAD(&sc.scanouts, &scanout, entries);
	pci_vgpu_set_legacy_scanout(&sc);
	assert(notifications == 1);
	assert(display_width == 800 && display_height == 600);
	assert(display_stride == 3200);

	/* GOP changes modes with separate 16-bit width and height writes. */
	pci_vgpu_legacy_ctrl_write(&pi, 0, 4, 2, 1920);
	pci_vgpu_legacy_ctrl_write(&pi, 0, 6, 2, 1080);
	assert(notifications == 3);
	assert(display_width == 1920 && display_height == 1080);
	assert(display_stride == 7680);
	assert(scanout.width == 1920 && scanout.height == 1080);
	assert(scanout.stride == 7680);

	/* Firmware rows are tightly packed, even for widths not divisible by 8. */
	pci_vgpu_legacy_ctrl_write(&pi, 0, 4, 2, 1366);
	pci_vgpu_legacy_ctrl_write(&pi, 0, 6, 2, 768);
	assert(notifications == 5);
	assert(display_width == 1366 && display_height == 768);
	assert(display_stride == 5464 && scanout.stride == 5464);
	assert(scanout.base_ptr == backing && backing[0] == 0x5a);

	/* Reject unusable layouts and ignore writes to unrelated registers. */
	pci_vgpu_legacy_ctrl_write(&pi, 0, 8, 2, 32);
	pci_vgpu_legacy_ctrl_write(&pi, 0, 4, 2, 0);
	pci_vgpu_legacy_ctrl_write(&pi, 0, 4, 2, COLS_MAX + 1);
	assert(notifications == 5);
	assert(display_width == 1366 && display_height == 768);
	sc.resx = 800;
	scanout.size = 1;
	pci_vgpu_set_legacy_scanout(&sc);
	assert(notifications == 5);

	puts("Legacy framebuffer mode changes and GOP stride passed");
	return (0);
}
