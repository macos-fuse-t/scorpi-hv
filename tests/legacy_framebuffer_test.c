/* SPDX-License-Identifier: BSD-2-Clause */
#include "pci_emul.h"
#undef PCI_EMUL_SET
#define PCI_EMUL_SET(x)
#include "../src/pci_virtio_gpu_vhost.c"

uuid_t vm_uuid;
int raw_stdio;
static unsigned int maps, unmaps;
static uint64_t mapped_gpa;

int
vm_setup_memory_segment(struct vmctx *ctx, vm_paddr_t gpa, size_t size,
    uint64_t prot, uintptr_t *addr)
{
	(void)ctx;
	assert(size == VHOST_LEGACY_FB_SIZE);
	assert((prot & PROT_DONT_ALLOCATE) != 0 && *addr != 0);
	assert(mapped_gpa == 0);
	mapped_gpa = gpa;
	maps++;
	return (0);
}

int
vm_munmap_memseg(struct vmctx *ctx, vm_paddr_t gpa, size_t size)
{
	(void)ctx;
	assert(size == VHOST_LEGACY_FB_SIZE && gpa == mapped_gpa);
	mapped_gpa = 0;
	unmaps++;
	return (0);
}

void
pci_emul_set_bar_direct_mapped(struct pci_devinst *pi, int idx, int enabled)
{
	(void)pi; (void)idx; (void)enabled;
	assert(false); /* No mapping failure expected. */
}

void
console_set_scanout(bool active, int w, int h, int stride, uint32_t format,
    const char *name, size_t size, bool legacy)
{
	(void)active; (void)w; (void)h; (void)stride; (void)format;
	(void)name; (void)size; (void)legacy;
}

void
console_update_scanout_rect(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

int
main(void)
{
	struct pci_devinst pi = {0};
	struct pci_vhost_softc sc = {0};
	uint8_t backing[64] = {0x5a};
	const uint64_t boot = 0xa0000000, moved = 0xb0000000;

	pi.pi_arg = &sc;
	pi.pi_bar[1].flags = PCIBAR_F_DIRECT_MAPPED;
	sc.vsc_vs.vs_pi = &pi;
	sc.legacy_fb_base = backing;
	sc.legacy_fb_size = sizeof(backing);
	pci_set_cfgdata16(&pi, PCIR_COMMAND, PCIM_CMD_MEMEN);
	pci_vhost_baraddr(&pi, 1, 1, boot);
	assert(maps == 1 && unmaps == 0 && mapped_gpa == boot);

	/* BasicDisplay's firmware mapping remains valid during driver loading. */
	pci_set_cfgdata16(&pi, PCIR_COMMAND, 0);
	pci_vhost_baraddr(&pi, 1, 0, boot);
	assert(sc.legacy_fb_direct_mapped && mapped_gpa == boot);
	assert(unmaps == 0 && backing[0] == 0x5a);
	pci_vhost_baraddr(&pi, 0, 0, boot); /* Control BAR is unaffected. */
	assert(mapped_gpa == boot);
	pci_set_cfgdata16(&pi, PCIR_COMMAND, PCIM_CMD_MEMEN);
	pci_vhost_baraddr(&pi, 1, 1, boot);
	assert(maps == 1 && unmaps == 0 && backing[0] == 0x5a);

	/* A BAR moved while decoding was off must retire its old mapping. */
	pci_set_cfgdata16(&pi, PCIR_COMMAND, 0);
	pci_vhost_baraddr(&pi, 1, 0, boot);
	pci_set_cfgdata16(&pi, PCIR_COMMAND, PCIM_CMD_MEMEN);
	pci_vhost_baraddr(&pi, 1, 1, moved);
	assert(maps == 2 && unmaps == 1 && mapped_gpa == moved);

	/* Relocation with decoding on and final teardown also remove mappings. */
	pci_vhost_baraddr(&pi, 1, 0, moved);
	assert(!sc.legacy_fb_direct_mapped && mapped_gpa == 0);
	pci_vhost_baraddr(&pi, 1, 1, boot);
	assert(maps == 3 && unmaps == 2 && mapped_gpa == boot);
	pci_vhost_unmap_legacy_fb(&sc);
	assert(unmaps == 3 && mapped_gpa == 0);
	puts("Legacy framebuffer mapping lifecycle passed");
	return (0);
}
