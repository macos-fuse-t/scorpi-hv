#pragma once

#include <stdbool.h>
#include <stdint.h>

bool host_display_info(uint32_t *logical_width, uint32_t *logical_height,
    uint32_t *pixel_width, uint32_t *pixel_height);
bool host_display_physical_size(uint32_t *width_mm, uint32_t *height_mm);
uint32_t host_display_scale(void);
