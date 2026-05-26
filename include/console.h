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

#ifndef _CONSOLE_H_
#define	_CONSOLE_H_

#include <stdbool.h>

struct bhyvegc;

typedef void (*fb_render_func_t)(void *arg);
typedef void (*kbd_event_func_t)(int down, uint8_t hidcode, uint8_t modifiers, void *arg);
typedef void (*ptr_event_func_t)(uint8_t mask, int x, int y, void *arg);
typedef void (*resize_event_func_t)(int x, int y, void *arg);

void console_init();

bool console_is_active();

int console_scanout_size(int *w, int *h);

void console_set_hdpi(bool hdpi);

void console_set_scanout(bool scanout_active, int w, int h, int sride,
    uint32_t format, const char *shm_name, size_t shm_size,
    bool redrawOnTimer);

void  console_set_mouse_scanout(bool scanout_active, int w, int h, uint32_t format, 
	int hot_x, int hot_y, const char *shm_name);

struct bhyvegc_image *console_get_image(void);

void console_fb_register(fb_render_func_t render_cb, void *arg);
void console_refresh(void);

void console_kbd_register(kbd_event_func_t event_cb, void *arg, int pri);
void console_key_event(int down, uint32_t keysym, uint32_t keycode);

void console_ptr_register(ptr_event_func_t event_cb, void *arg, int pri);
void console_ptr_event(uint8_t button, int x, int y);
void console_resize_register(resize_event_func_t event_cb, void *arg);

#endif /* _CONSOLE_H_ */
