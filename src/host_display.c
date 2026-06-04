#include <sys/param.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif

#include "host_display.h"

bool
host_display_info(uint32_t *logical_width, uint32_t *logical_height,
    uint32_t *pixel_width, uint32_t *pixel_height)
{
#ifdef __APPLE__
	CGDirectDisplayID display;
	CGDisplayModeRef mode;

	display = CGMainDisplayID();
	mode = CGDisplayCopyDisplayMode(display);
	if (mode == NULL)
		return (false);

	*logical_width = (uint32_t)CGDisplayModeGetWidth(mode);
	*logical_height = (uint32_t)CGDisplayModeGetHeight(mode);
	*pixel_width = (uint32_t)CGDisplayModeGetPixelWidth(mode);
	*pixel_height = (uint32_t)CGDisplayModeGetPixelHeight(mode);
	CGDisplayModeRelease(mode);

	return (*logical_width > 0 && *logical_height > 0 && *pixel_width > 0 &&
	    *pixel_height > 0);
#else
	(void)logical_width;
	(void)logical_height;
	(void)pixel_width;
	(void)pixel_height;
	return (false);
#endif
}

bool
host_display_physical_size(uint32_t *width_mm, uint32_t *height_mm)
{
#ifdef __APPLE__
	CGSize size;

	size = CGDisplayScreenSize(CGMainDisplayID());
	if (size.width <= 0 || size.height <= 0)
		return (false);

	*width_mm = (uint32_t)size.width;
	*height_mm = (uint32_t)size.height;
	return (*width_mm >= 100 && *height_mm >= 80);
#else
	(void)width_mm;
	(void)height_mm;
	return (false);
#endif
}

uint32_t
host_display_scale(void)
{
	uint32_t logical_width;
	uint32_t logical_height;
	uint32_t pixel_width;
	uint32_t pixel_height;
	uint32_t scale;

	if (!host_display_info(&logical_width, &logical_height, &pixel_width,
		&pixel_height)) {
		return (1);
	}

	scale = MIN(pixel_width / logical_width, pixel_height / logical_height);
	return (MAX(scale, 1));
}
