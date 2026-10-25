/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015 Tycho Nightingale <tycho.nightingale@pluribusnetworks.com>
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bhyvegc.h"
#include "cnc.h"
#include "console.h"
#include "debug.h"

#define BUFSIZE		     1024
#define CONSOLE_SHM_NAME_MAX 256

static struct {
	// struct bhyvegc		*gc;

	fb_render_func_t fb_render_cb;
	void *fb_arg;

	kbd_event_func_t kbd_event_cb;
	void *kbd_arg;
	int kbd_priority;

	ptr_event_func_t ptr_event_cb;
	void *ptr_arg;
	int ptr_priority;

	scroll_event_func_t scroll_event_cb;
	void *scroll_arg;
	int scroll_priority;

	relative_event_func_t relative_event_cb;
	void *relative_arg;

	resize_event_func_t resize_event_cb;
	void *resize_arg;

	bool scanout_active;
	uint32_t format;
	int w;
	int h;
	int stride;
	char shm_name[CONSOLE_SHM_NAME_MAX];
	size_t shm_size;
	bool redrawOnTimer;
	bool display_surface_active;
	uint32_t display_surface_format;
	uint32_t display_surface_handle_type;
	uint64_t display_surface_id;
	uint64_t display_surface_fence_value;
	uint64_t display_surface_diagnostic_sequence;
	bool hdpi;

	bool hardware_mouse;
	uint32_t mouse_format;
	int mouse_w;
	int mouse_h;
	int mouse_stride;
	int mouse_hot_x;
	int mouse_hot_y;
	int mouse_x;
	int mouse_y;
	char mouse_shm_name[CONSOLE_SHM_NAME_MAX];
	bool mouse_active;
} console;

void
console_set_hdpi(bool hdpi)
{
	console.hdpi = hdpi;
}

void
console_set_hardware_mouse(bool hardware_mouse)
{
	console.hardware_mouse = hardware_mouse;
}

void
console_set_scanout(bool scanout_active, int w, int h, int stride,
    uint32_t format, const char *shm_name, size_t shm_size, bool redrawOnTimer)
{
	char data[BUFSIZE];

	console.scanout_active = scanout_active;
	console.w = w;
	console.h = h;
	console.stride = stride;
	console.format = format;
	if (scanout_active && shm_name != NULL)
		snprintf(console.shm_name, sizeof(console.shm_name), "%s",
		    shm_name);
	else
		console.shm_name[0] = '\0';
	console.shm_size = shm_size;
	console.redrawOnTimer = redrawOnTimer;

	if (scanout_active)
		snprintf(data, sizeof(data),
		    "{ \"event\": \"set_scanout\", "
		    "\"data\": { "
		    "\"width\": %d, "
		    "\"height\": %d, "
		    "\"stride\": %d, "
		    "\"format\": %d, "
		    "\"shm_name\": \"%s\", "
		    "\"shm_size\": %zu, "
		    "\"redrawOnTimer\": %s"
		    "}"
		    "}",
		    w, h, stride, format, console.shm_name, shm_size,
		    redrawOnTimer ? "true" : "false");
	else
		snprintf(data, sizeof(data), "{ \"event\": \"unset_scanout\"}");
	cnc_send_notification(data);
}

void
console_set_display_surface(bool active, int w, int h, uint32_t format,
    uint32_t handle_type, uint64_t surface_id, uint64_t fence_value,
    uint64_t diagnostic_sequence)
{
	char data[BUFSIZE];

	console.display_surface_active = active;
	console.w = w;
	console.h = h;
	console.display_surface_format = format;
	console.display_surface_handle_type = handle_type;
	console.display_surface_id = surface_id;
	console.display_surface_fence_value = fence_value;
	console.display_surface_diagnostic_sequence = diagnostic_sequence;

	if (active) {
		snprintf(data, sizeof(data),
		    "{ \"event\": \"set_display_surface\", "
		    "\"data\": { "
		    "\"width\": %d, "
		    "\"height\": %d, "
		    "\"format\": %u, "
		    "\"handle_type\": %u, "
		    "\"surface_id\": %llu, "
		    "\"fence_value\": %llu, "
		    "\"diagnostic_sequence\": %llu"
		    "}"
		    "}",
		    w, h, format, handle_type,
		    (unsigned long long)surface_id,
		    (unsigned long long)fence_value,
		    (unsigned long long)diagnostic_sequence);
	} else {
		snprintf(data, sizeof(data),
		    "{ \"event\": \"unset_display_surface\"}");
	}
	if (diagnostic_sequence != 0) {
		EPRINTLN(
		    "PRESENT_DIAG console-notify sequence=%llu active=%u surface=%llu size=%dx%d",
		    (unsigned long long)diagnostic_sequence,
		    active ? 1u : 0u,
		    (unsigned long long)surface_id, w, h);
	}
	cnc_send_notification(data);
}

void
console_update_scanout_rect(int x, int y, int w, int h)
{
	char notification[BUFSIZE];

	snprintf(notification, sizeof(notification),
	    "{ \"event\": \"update_scanout\", "
	    "\"data\": {"
	    "\"x\": %d,"
	    "\"y\": %d,"
	    "\"width\": %d,"
	    "\"height\": %d"
	    "}"
	    "}",
	    x, y, w, h);
	cnc_send_notification(notification);
}

void
console_set_mouse_scanout(bool scanout_active, int w, int h, int stride,
    uint32_t format, int hot_x, int hot_y, const char *shm_name)
{
	char notification[1024];

	console.hardware_mouse = true;
	console.mouse_active = scanout_active;
	console.mouse_w = w;
	console.mouse_h = h;
	console.mouse_stride = stride;
	console.mouse_format = format;
	if (scanout_active && shm_name != NULL)
		snprintf(console.mouse_shm_name, sizeof(console.mouse_shm_name),
		    "%s", shm_name);
	else
		console.mouse_shm_name[0] = '\0';
	console.mouse_hot_x = hot_x;
	console.mouse_hot_y = hot_y;

	if (!scanout_active)
		snprintf(notification, sizeof(notification),
		    "{\"event\": \"hide_cursor\"}");
	else
		snprintf(notification, sizeof(notification),
		    "{ \"event\": \"update_cursor\","
		    "\"data\": {"
		    "\"hot_x\": %d,"
		    "\"hot_y\": %d,"
		    "\"width\": %d, "
		    "\"height\": %d, "
		    "\"stride\": %d, "
		    "\"format\": %d, "
		    "\"shm_name\": \"%s\""
		    "}"
		    "}",
		    hot_x, hot_y, w, h, stride, format, console.mouse_shm_name);

	cnc_send_notification(notification);
}

void
console_move_cursor(int x, int y)
{
	char notification[1024];

	console.mouse_x = x;
	console.mouse_y = y;

	snprintf(notification, sizeof(notification),
	    "{ \"event\": \"move_cursor\","
	    "\"data\": {"
	    "\"x\": %d,"
	    "\"y\": %d"
	    "}"
	    "}",
	    x, y);
	cnc_send_notification(notification);
}

struct bhyvegc_image *
console_get_image(void)
{
	/*struct bhyvegc_image *bhyvegc_image;

	bhyvegc_image = bhyvegc_get_image(console.gc);

	return (bhyvegc_image);*/
	return (NULL);
}

bool
console_is_active()
{
	return console.scanout_active || console.display_surface_active;
}

int
console_scanout_size(int *w, int *h)
{
	if (!console_is_active())
		return (-1);

	*w = console.w;
	*h = console.h;
	return (0);
}

void
console_fb_register(fb_render_func_t render_cb, void *arg)
{
	console_init();
	console.fb_render_cb = render_cb;
	console.fb_arg = arg;
}

void
console_refresh(void)
{
	if (console.fb_render_cb)
		(*console.fb_render_cb)(console.fb_arg);
}

void
console_kbd_register(kbd_event_func_t event_cb, void *arg, int pri)
{
	if (pri > console.kbd_priority) {
		console.kbd_event_cb = event_cb;
		console.kbd_arg = arg;
		console.kbd_priority = pri;
	}
}

void
console_ptr_register(ptr_event_func_t event_cb, void *arg, int pri)
{
	if (pri > console.ptr_priority) {
		console.ptr_event_cb = event_cb;
		console.ptr_arg = arg;
		console.ptr_priority = pri;
	}
}

void
console_resize_register(resize_event_func_t event_cb, void *arg)
{
	console.resize_event_cb = event_cb;
	console.resize_arg = arg;
}

void
console_scroll_register(scroll_event_func_t event_cb, void *arg, int pri)
{
	if (pri > console.scroll_priority) {
		console.scroll_event_cb = event_cb;
		console.scroll_arg = arg;
		console.scroll_priority = pri;
	}
}

void
console_scroll_event(uint8_t button, int x, int y, int delta)
{
	if (console.scroll_event_cb)
		(*console.scroll_event_cb)(button, x, y, delta, console.scroll_arg);
}

void
console_relative_register(relative_event_func_t event_cb, void *arg)
{
	console.relative_event_cb = event_cb;
	console.relative_arg = arg;
}

void
console_relative_event(uint8_t button, int dx, int dy, int scroll)
{
	if (console.relative_event_cb)
		console.relative_event_cb(button, dx, dy, scroll,
		    console.relative_arg);
}

void
console_key_event(int down, uint32_t keysym, uint32_t keycode)
{
	if (console.kbd_event_cb)
		(*console.kbd_event_cb)(down, keysym, keycode, console.kbd_arg);
}

void
console_ptr_event(uint8_t button, int x, int y)
{
	if (console.ptr_event_cb)
		(*console.ptr_event_cb)(button, x, y, console.ptr_arg);
}

void
console_resize_event(int x, int y)
{
	if (console.resize_event_cb)
		(*console.resize_event_cb)(x, y, console.resize_arg);
}

static void
get_framebuffer(cnc_conn_t c, int req_id, int argc, char *argv[], void *param)
{
	char rsp[1024];

	snprintf(rsp, sizeof(rsp),
	    "{"
	    "\"hardware_mouse\": %s, "
	    "\"hdpi\": %s, "
	    "\"relative_mouse\": %s, "
	    "\"display_surface\": {"
	    "\"active\": %s, "
	    "\"width\": %d, "
	    "\"height\": %d, "
	    "\"format\": %u, "
	    "\"handle_type\": %u, "
	    "\"surface_id\": %llu, "
	    "\"fence_value\": %llu"
	    "},"
	    "\"scanout\": {"
	    "\"width\": %d, "
	    "\"height\": %d, "
	    "\"stride\": %d, "
	    "\"format\": %d, "
	    "\"shm_name\": \"%s\", "
	    "\"shm_size\": %zu, "
	    "\"redrawOnTimer\": %s"
	    "},"
	    "\"mouse_scanout\": {"
	    "\"x\": %d,\"y\": %d,"
	    "\"hot_x\": %d,"
	    "\"hot_y\": %d,"
	    "\"width\": %d, "
	    "\"height\": %d, "
	    "\"stride\": %d, "
	    "\"format\": %d, "
	    "\"shm_name\": \"%s\""
	    "}"
	    "}",
	    console.hardware_mouse ? "true" : "false",
	    console.hdpi ? "true" : "false",
	    console.relative_event_cb ? "true" : "false",
	    console.display_surface_active ? "true" : "false", console.w,
	    console.h, console.display_surface_format,
	    console.display_surface_handle_type,
	    (unsigned long long)console.display_surface_id,
	    (unsigned long long)console.display_surface_fence_value, console.w,
	    console.h, console.stride, console.format, console.shm_name,
	    console.shm_size, console.redrawOnTimer ? "true" : "false",
	    console.mouse_x, console.mouse_y,
	    console.mouse_hot_x, console.mouse_hot_y, console.mouse_w,
	    console.mouse_h, console.mouse_stride, console.mouse_format,
	    console.mouse_shm_name);
	cnc_send_response(c, req_id, rsp);
}

static void
resize_framebuffer(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	int x, y;

	if (argc >= 2) {
		x = atoi(argv[0]);
		y = atoi(argv[1]);
		console_resize_event(x, y);
	}
}

static void
mouse_event(cnc_conn_t c, int req_id, int argc, char *argv[], void *param)
{
	int button, x, y;
	if (argc > 2) {
		button = atoi(argv[0]);
		x = atoi(argv[1]);
		y = atoi(argv[2]);
		console_ptr_event(button, x, y);
	}
}

static void
kbd_event(cnc_conn_t c, int req_id, int argc, char *argv[], void *param)
{
	int down, hidcode, mods;
	if (argc > 2) {
		down = atoi(argv[0]);
		hidcode = atoi(argv[1]);
		mods = atoi(argv[2]);
		console_key_event(down, hidcode, mods);
	}
}

static bool
parse_pointer_args(int argc, char *argv[], int values[4])
{
	char *end;

	if (argc != 4)
		return (false);
	for (int i = 0; i < 4; i++) {
		errno = 0;
		long value = strtol(argv[i], &end, 10);
		if (errno || end == argv[i] || *end != '\0' ||
		    value < INT_MIN || value > INT_MAX)
			return (false);
		values[i] = value;
	}
	return (true);
}

static void
scroll_event(cnc_conn_t c, int req_id, int argc, char *argv[], void *param)
{
	int values[4];

	if (parse_pointer_args(argc, argv, values))
		console_scroll_event(values[0], values[1], values[2],
		    values[3]);
}

static void
relative_mouse_event(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	int values[4];

	if (parse_pointer_args(argc, argv, values) && values[0] >= 0 &&
	    values[0] <= 7)
		console_relative_event(values[0], values[1], values[2],
		    values[3]);
}

void
console_init()
{
	static int once = 0;

	if (!once) {
		cnc_register_command("get_framebuffer", get_framebuffer, NULL);
		cnc_register_command("resize_framebuffer", resize_framebuffer,
		    NULL);
		cnc_register_command("mouse_event", mouse_event, NULL);
		cnc_register_command("scroll_event", scroll_event, NULL);
		cnc_register_command("relative_mouse_event",
		    relative_mouse_event, NULL);
		cnc_register_command("key_event", kbd_event, NULL);
	}
	once = 1;
}
