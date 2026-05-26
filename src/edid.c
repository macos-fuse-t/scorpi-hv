/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <support/endian.h>

#include "compat.h"
#include "edid.h"

#define SCORPI_EDID_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SCORPI_EDID_DPI 96
#define SCORPI_EDID_MM_PER_INCH_X10 254
#define SCORPI_EDID_MIN(a, b) ((a) < (b) ? (a) : (b))
#define SCORPI_EDID_MAX(a, b) ((a) > (b) ? (a) : (b))

struct edid_mode {
	uint16_t width;
	uint16_t height;
	uint16_t pixel_clock;
	uint16_t hblank;
	uint16_t vblank;
	uint16_t hsync_offset;
	uint16_t hsync_width;
	uint16_t vsync_offset;
	uint16_t vsync_width;
	uint8_t misc;
};

static const struct edid_mode edid_modes[] = {
	{ 640, 480, 2518, 160, 45, 16, 96, 10, 2, DRM_EDID_PT_SEPARATE_SYNC },
	{ 800, 600, 4000, 256, 28, 40, 128, 1, 4,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 1024, 768, 6500, 320, 38, 24, 136, 3, 6,
	    DRM_EDID_PT_SEPARATE_SYNC },
	{ 1280, 720, 7425, 370, 30, 110, 40, 5, 5,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 1280, 1024, 10800, 408, 42, 48, 112, 1, 3,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 1366, 768, 8550, 426, 30, 70, 143, 3, 3,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 1600, 900, 10800, 160, 26, 48, 32, 3, 5,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 1920, 1080, 14850, 280, 45, 88, 44, 4, 5,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 2560, 1440, 24150, 160, 41, 48, 32, 3, 5,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 2880, 1800, 33000, 160, 43, 48, 32, 3, 5,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
	{ 3840, 2160, 53325, 160, 55, 48, 32, 3, 5,
	    DRM_EDID_PT_SEPARATE_SYNC | DRM_EDID_PT_HSYNC_POSITIVE |
		DRM_EDID_PT_VSYNC_POSITIVE },
};

static void
edid_mfg_id(uint8_t mfg_id[2], const char name[3])
{
	uint16_t id;

	id = ((name[0] - '@') & 0x1f) << 10;
	id |= ((name[1] - '@') & 0x1f) << 5;
	id |= (name[2] - '@') & 0x1f;

	mfg_id[0] = id >> 8;
	mfg_id[1] = id & 0xff;
}

static const struct edid_mode *
edid_find_mode(uint16_t width, uint16_t height)
{
	for (size_t i = 0; i < SCORPI_EDID_ARRAY_SIZE(edid_modes); i++) {
		if (edid_modes[i].width == width &&
		    edid_modes[i].height == height)
			return (&edid_modes[i]);
	}

	return (NULL);
}

static struct edid_mode
edid_fallback_mode(uint16_t width, uint16_t height)
{
	uint32_t pixel_clock;

	pixel_clock = ((uint32_t)width + 160) * ((uint32_t)height + 45) *
	    60 / 10000;
	return ((struct edid_mode) {
		.width = width,
		.height = height,
		.pixel_clock = SCORPI_EDID_MIN(pixel_clock, UINT16_MAX),
		.hblank = 160,
		.vblank = 45,
		.hsync_offset = 48,
		.hsync_width = 32,
		.vsync_offset = 3,
		.vsync_width = 5,
		.misc = DRM_EDID_PT_SEPARATE_SYNC |
		    DRM_EDID_PT_HSYNC_POSITIVE |
		    DRM_EDID_PT_VSYNC_POSITIVE,
	});
}

static struct edid_mode
edid_mode(uint16_t width, uint16_t height)
{
	const struct edid_mode *mode;

	mode = edid_find_mode(width, height);
	if (mode != NULL)
		return (*mode);

	return (edid_fallback_mode(width, height));
}

static void
edid_detailed_timing(struct detailed_timing *dt,
    const struct edid_mode *mode, uint16_t width_mm, uint16_t height_mm)
{
	struct detailed_pixel_timing *pd;
	uint16_t hblank, vblank;
	uint16_t hsync_offset, hsync_width;
	uint16_t vsync_offset, vsync_width;

	bzero(dt, sizeof(*dt));
	hblank = mode->hblank;
	vblank = mode->vblank;
	hsync_offset = mode->hsync_offset;
	hsync_width = mode->hsync_width;
	vsync_offset = mode->vsync_offset;
	vsync_width = mode->vsync_width;

	dt->pixel_clock = htole16(mode->pixel_clock);
	pd = &dt->data.pixel_data;

	pd->hactive_lo = mode->width & 0xff;
	pd->hblank_lo = hblank & 0xff;
	pd->hactive_hblank_hi = ((mode->width >> 8) & 0xf) << 4 |
	    ((hblank >> 8) & 0xf);
	pd->vactive_lo = mode->height & 0xff;
	pd->vblank_lo = vblank & 0xff;
	pd->vactive_vblank_hi = ((mode->height >> 8) & 0xf) << 4 |
	    ((vblank >> 8) & 0xf);
	pd->hsync_offset_lo = hsync_offset & 0xff;
	pd->hsync_pulse_width_lo = hsync_width & 0xff;
	pd->vsync_offset_pulse_width_lo = ((vsync_offset & 0xf) << 4) |
	    (vsync_width & 0xf);
	pd->hsync_vsync_offset_pulse_width_hi =
	    ((hsync_offset >> 8) & 0x3) << 6 |
	    ((hsync_width >> 8) & 0x3) << 4 |
	    ((vsync_offset >> 4) & 0x3) << 2 |
	    ((vsync_width >> 4) & 0x3);
	pd->width_mm_lo = width_mm & 0xff;
	pd->height_mm_lo = height_mm & 0xff;
	pd->width_height_mm_hi = ((width_mm >> 8) & 0xf) << 4 |
	    ((height_mm >> 8) & 0xf);
	pd->misc = mode->misc;
}

static void
edid_standard_timing(struct std_timing *timing, uint16_t width,
    uint8_t aspect)
{
	timing->hsize = (width / 8) - 31;
	timing->vfreq_aspect = (aspect << EDID_TIMING_ASPECT_SHIFT);
}

static bool
edid_timing_aspect(uint16_t width, uint16_t height, uint8_t *aspect)
{
	if ((uint32_t)width * 10 == (uint32_t)height * 16) {
		*aspect = 0;
		return (true);
	}
	if ((uint32_t)width * 3 == (uint32_t)height * 4) {
		*aspect = 1;
		return (true);
	}
	if ((uint32_t)width * 4 == (uint32_t)height * 5) {
		*aspect = 2;
		return (true);
	}
	if ((uint32_t)width * 9 == (uint32_t)height * 16) {
		*aspect = 3;
		return (true);
	}

	return (false);
}

static bool
edid_mode_valid(uint32_t width, uint32_t height, uint32_t max_width,
    uint32_t max_height)
{
	return (width >= 640 && width <= max_width &&
	    height >= 480 && height <= max_height);
}

static bool
edid_mode_exists(const struct edid_mode *modes, size_t count,
    uint16_t width, uint16_t height)
{
	for (size_t i = 0; i < count; i++) {
		if (modes[i].width == width && modes[i].height == height)
			return (true);
	}

	return (false);
}

static void
edid_add_mode(struct edid_mode *modes, size_t max_modes,
    size_t *count, uint32_t width, uint32_t height, uint32_t max_width,
    uint32_t max_height)
{
	if (*count >= max_modes ||
	    !edid_mode_valid(width, height, max_width, max_height))
		return;
	if (edid_mode_exists(modes, *count, width, height))
		return;

	modes[*count] = edid_mode(width, height);
	(*count)++;
}

static uint32_t
edid_roundup(uint32_t value, uint32_t align)
{
	return ((value + align - 1) & ~(align - 1));
}

static void
edid_add_scaled_mode(struct edid_mode *modes, size_t max_modes,
    size_t *count, uint16_t width, uint16_t height, uint32_t scale_percent,
    uint32_t max_width, uint32_t max_height)
{
	uint32_t scaled_width;
	uint32_t scaled_height;

	scaled_width = (uint32_t)width * scale_percent / 100;
	scaled_height = (uint32_t)height * scale_percent / 100;
	scaled_width = edid_roundup(scaled_width, 8);
	scaled_height = edid_roundup(scaled_height, 8);

	edid_add_mode(modes, max_modes, count, scaled_width,
	    scaled_height, max_width, max_height);
}

static void
edid_populate_standard_timings(struct edid *edid,
    const struct edid_mode *modes, size_t mode_count)
{
	size_t timing_index;
	uint8_t aspect;

	for (timing_index = 0;
	     timing_index < SCORPI_EDID_ARRAY_SIZE(edid->standard_timings);
	     timing_index++) {
		edid->standard_timings[timing_index].hsize = 0x01;
		edid->standard_timings[timing_index].vfreq_aspect = 0x01;
	}

	timing_index = 0;
	for (size_t mode_index = 0; mode_index < mode_count &&
	     timing_index < SCORPI_EDID_ARRAY_SIZE(edid->standard_timings);
	     mode_index++) {
		uint16_t width = modes[mode_index].width;
		uint16_t height = modes[mode_index].height;

		if (width < 256 || width > 2288 || width % 8 != 0)
			continue;
		if (!edid_timing_aspect(width, height, &aspect))
			continue;

		edid_standard_timing(
		    &edid->standard_timings[timing_index], width, aspect);
		timing_index++;
	}
}

static void
edid_populate_detailed_timings(struct edid *edid,
    const struct edid_mode *modes, size_t mode_count, uint16_t width_mm,
    uint16_t height_mm)
{
	size_t count;

	count = SCORPI_EDID_MIN(mode_count,
	    SCORPI_EDID_ARRAY_SIZE(edid->detailed_timings));
	for (size_t i = 0; i < count; i++) {
		edid_detailed_timing(&edid->detailed_timings[i],
		    &modes[i], width_mm, height_mm);
	}
}

static void
edid_checksum(uint8_t *block)
{
	uint8_t sum;

	block[127] = 0;
	sum = 0;
	for (size_t i = 0; i < 127; i++)
		sum += block[i];
	block[127] = (256 - sum) % 256;
}

int
generate_edid(uint16_t width, uint16_t height, uint16_t logical_width,
    uint16_t logical_height, uint16_t physical_width_mm,
    uint16_t physical_height_mm, bool hdpi_enabled, uint32_t max_width,
    uint32_t max_height, uint8_t *edid_buf)
{
	struct edid *edid;
	struct edid_mode modes[8];
	size_t mode_count;
	uint16_t width_mm, height_mm;
	uint32_t serial;

	edid = (struct edid *)edid_buf;
	mode_count = 0;
	if (!edid_mode_valid(logical_width, logical_height,
		max_width, max_height)) {
		logical_width = width;
		logical_height = height;
	}

	if (physical_width_mm >= 100 && physical_height_mm >= 80) {
		width_mm = physical_width_mm;
		height_mm = physical_height_mm;
	} else {
		width_mm = SCORPI_EDID_MAX((uint32_t)logical_width *
		    SCORPI_EDID_MM_PER_INCH_X10 / (SCORPI_EDID_DPI * 10),
		    100);
		height_mm = SCORPI_EDID_MAX((uint32_t)logical_height *
		    SCORPI_EDID_MM_PER_INCH_X10 / (SCORPI_EDID_DPI * 10),
		    80);
	}
	serial = ((uint32_t)width << 16) ^ height ^
	    ((uint32_t)width_mm << 8) ^ height_mm;

	edid_add_mode(modes, SCORPI_EDID_ARRAY_SIZE(modes),
	    &mode_count, width, height, max_width, max_height);
	edid_add_mode(modes, SCORPI_EDID_ARRAY_SIZE(modes),
	    &mode_count, logical_width, logical_height, max_width,
	    max_height);
	(void)hdpi_enabled;
	edid_add_mode(modes, SCORPI_EDID_ARRAY_SIZE(modes), &mode_count,
	    max_width, max_height, max_width, max_height);
	edid_add_scaled_mode(modes, SCORPI_EDID_ARRAY_SIZE(modes),
	    &mode_count, logical_width, logical_height, 125, max_width,
	    max_height);
	edid_add_scaled_mode(modes, SCORPI_EDID_ARRAY_SIZE(modes),
	    &mode_count, logical_width, logical_height, 150, max_width,
	    max_height);
	edid_add_scaled_mode(modes, SCORPI_EDID_ARRAY_SIZE(modes),
	    &mode_count, logical_width, logical_height, 175, max_width,
	    max_height);

	memset(edid_buf, 0, EDID_LENGTH);
	*edid = (struct edid) {
			.header = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
			    0xFF, 0x00 },
			.prod_code = { 0x04, 0x00 },
			.serial = { serial & 0xff, (serial >> 8) & 0xff,
			    (serial >> 16) & 0xff, (serial >> 24) & 0xff },
		.mfg_week = 0x01,
		.mfg_year = 0x23,
		.version = 0x01,
		.revision = 0x04,
		.input = 0x80,
		.width_cm = width_mm / 10,
		.height_cm = height_mm / 10,
		.gamma = 120,
		.features = DRM_EDID_FEATURE_PREFERRED_TIMING |
		    DRM_EDID_FEATURE_STANDARD_COLOR,
		.red_green_lo = 0x20,
		.black_white_lo = 0x20,
		.red_x = 0xA4,
		.red_y = 0x56,
		.green_x = 0x34,
		.green_y = 0x78,
		.blue_x = 0x12,
		.blue_y = 0x9A,
		.white_x = 0xC8,
		.white_y = 0x32,
		.established_timings = {
			.t1 = 0x21,
			.t2 = 0x08,
		},
		.extensions = 0x00,
		.checksum = 0x00,
	};

	edid_mfg_id(edid->mfg_id, "SCR");
	edid_populate_standard_timings(edid, modes, mode_count);
	edid_populate_detailed_timings(edid, modes, mode_count,
	    width_mm, height_mm);
	edid_checksum(edid_buf);

	return (EDID_LENGTH);
}
