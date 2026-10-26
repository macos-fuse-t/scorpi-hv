/* SPDX-License-Identifier: BSD-2-Clause */
#include "pci_emul.h"
#undef PCI_EMUL_SET
#define PCI_EMUL_SET(x)
#include "../src/pci_virtio_gpu_vhost.c"
#include "../src/virtio.c"

uuid_t vm_uuid;
int raw_stdio;
static bool surface_active;
static char events[64];
static unsigned event_count;

static void record(char event)
{
	assert(event_count < sizeof(events));
	events[event_count++] = event;
}

void console_set_display_surface(bool active, int w, int h, uint32_t format,
    uint32_t handle_type, uint64_t id, uint64_t fence, uint64_t sequence)
{
	(void)w; (void)h; (void)format; (void)handle_type;
	(void)id; (void)fence; (void)sequence;
	surface_active = active;
	record(active ? 'I' : 'U');
}

void console_set_scanout(bool active, int w, int h, int stride,
    uint32_t format, const char *name, size_t size, bool timer)
{
	(void)w; (void)h; (void)stride; (void)format; (void)size;
	assert(active && name);
	/* Viewer discards this notification if the old IOSurface is active. */
	assert(!surface_active);
	record(timer ? 'L' : 'S');
}

void console_update_scanout_rect(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

void pci_vhost_clear_features(struct pci_vhost_state *state)
{
	state->features = 0;
	state->features_negotiated = false;
}

void pci_vhost_advance_reset_generation(struct pci_vhost_state *state)
{
	state->reset_generation++;
}

int pci_vhost_bind_transport(struct pci_vhost_state *state, struct vmctx *ctx,
    const struct pci_vhost_transport_info *info)
{
	(void)state; (void)ctx; (void)info;
	return (0);
}

void pci_lintr_deassert(struct pci_devinst *pi) { (void)pi; }

int main(void)
{
	uint8_t firmware_pixels[16];
	struct pci_devinst pi = {0};
	struct pci_vhost_softc sc = {0};
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sc.vsc_mtx, &attr);
	pthread_mutexattr_destroy(&attr);
	sc.vsc_consts.vc_nvq = VHOST_GPU_QUEUE_COUNT;
	sc.vsc_consts.vc_reset = pci_vhost_reset;
	sc.vsc_vs.vs_vc = &sc.vsc_consts;
	sc.vsc_vs.vs_pi = &pi;
	sc.vsc_vs.vs_queues = sc.queues;
	sc.vsc_vs.vs_mtx = &sc.vsc_mtx;
	sc.vsc_vs.vs_status = VIRTIO_CONFIG_STATUS_DRIVER_OK;
	pi.pi_arg = &sc;
	sc.legacy_fb_base = firmware_pixels;
	sc.legacy_fb_size = sizeof(firmware_pixels);
	sc.resx = sc.resy = 2;
	snprintf(sc.legacy_fb_shm_name, sizeof(sc.legacy_fb_shm_name), "legacy");
	sc.renderer_surface_active = surface_active = true;
	memset(firmware_pixels, 0xaa, sizeof(firmware_pixels));
	vi_pci_reset(&pi);
	assert(!sc.renderer_surface_active && !sc.renderer_scanout_active);
	assert(event_count == 2 && events[0] == 'U' && events[1] == 'L');
	for (unsigned i = 0; i < sizeof(firmware_pixels); i++)
		assert(firmware_pixels[i] == 0);

	/* Late backend reset notices must preserve pixels written by firmware. */
	memset(firmware_pixels, 0x5a, sizeof(firmware_pixels));
	struct scorpi_vhost_display_surface surface = {0};
	struct scorpi_vhost_scanout scanout = {0};
	pci_vhost_apply_renderer_display_surface(&sc, &surface);
	pci_vhost_apply_renderer_scanout(&sc, &scanout);
	for (unsigned i = 0; i < sizeof(firmware_pixels); i++)
		assert(firmware_pixels[i] == 0x5a);

	/* Old frame publications cannot reclaim the display during boot. */
	unsigned before = event_count;
	surface.flags = SCORPI_VHOST_DISPLAY_SURFACE_F_ENABLED;
	surface.width = surface.height = 2;
	surface.surface_id = 1;
	scanout.flags = SCORPI_VHOST_SCANOUT_F_ENABLED;
	scanout.width = scanout.height = 2;
	scanout.shm_size = sizeof(firmware_pixels);
	snprintf(scanout.shm_name, sizeof(scanout.shm_name), "renderer");
	pci_vhost_apply_renderer_display_surface(&sc, &surface);
	pci_vhost_apply_renderer_scanout(&sc, &scanout);
	assert(event_count == before);

	/* New driver frames resume normally. IOSurface -> shared memory must
	 * also notify the source release before the replacement scanout. */
	sc.vsc_vs.vs_status = VIRTIO_CONFIG_STATUS_DRIVER_OK;
	pci_vhost_apply_renderer_display_surface(&sc, &surface);
	assert(sc.renderer_surface_active && surface_active);
	before = event_count;
	pci_vhost_apply_renderer_scanout(&sc, &scanout);
	assert(event_count == before + 2 && events[before] == 'U' &&
	    events[before + 1] == 'S');
	assert(sc.renderer_scanout_active && !sc.renderer_surface_active);
	/* A genuine renderer-to-firmware transition still clears old contents. */
	memset(firmware_pixels, 0x5a, sizeof(firmware_pixels));
	scanout.flags = 0;
	pci_vhost_apply_renderer_scanout(&sc, &scanout);
	for (unsigned i = 0; i < sizeof(firmware_pixels); i++)
		assert(firmware_pixels[i] == 0);
	pci_vhost_apply_renderer_display_surface(&sc, &surface);
	memset(firmware_pixels, 0x5a, sizeof(firmware_pixels));
	surface.flags = 0;
	pci_vhost_apply_renderer_display_surface(&sc, &surface);
	for (unsigned i = 0; i < sizeof(firmware_pixels); i++)
		assert(firmware_pixels[i] == 0);
	pthread_mutex_destroy(&sc.vsc_mtx);
	return (0);
}
