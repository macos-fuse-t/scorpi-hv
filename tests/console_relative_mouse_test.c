/* SPDX-License-Identifier: BSD-2-Clause */
#include "../src/console.c"
#include <assert.h>
#include <string.h>

static CMD_HANDLER relative_command;
static int calls, last_dx, last_dy, last_scroll;
static uint8_t last_buttons;
static char response[2048];

void
cnc_register_command(const char *name, CMD_HANDLER handler, void *arg)
{
	if (!strcmp(name, "relative_mouse_event"))
		relative_command = handler;
}

void
cnc_send_response(cnc_conn_t conn, int id, const char *data)
{
	snprintf(response, sizeof(response), "%s", data);
}

void
cnc_send_notification(const char *data) {}

static void
relative_input(uint8_t buttons, int dx, int dy, int scroll, void *arg)
{
	calls++;
	last_buttons = buttons;
	last_dx = dx;
	last_dy = dy;
	last_scroll = scroll;
}

int
main(void)
{
	console_init();
	assert(relative_command);
	get_framebuffer(NULL, 1, 0, NULL, NULL);
	assert(strstr(response, "\"relative_mouse\": false"));
	console_relative_register(relative_input, NULL);
	get_framebuffer(NULL, 1, 0, NULL, NULL);
	assert(strstr(response, "\"relative_mouse\": true"));
	char *args[] = {"3", "-1000", "2000", "-120"};
	relative_command(NULL, 1, 4, args, NULL);
	assert(calls == 1 && last_buttons == 3 && last_dx == -1000 &&
	    last_dy == 2000 && last_scroll == -120);
	for (unsigned i = 0; i < 4; i++) {
		char *saved = args[i];
		args[i] = "1.5";
		relative_command(NULL, 1, 4, args, NULL);
		args[i] = "999999999999999999999999999";
		relative_command(NULL, 1, 4, args, NULL);
		args[i] = saved;
	}
	args[0] = "8";
	relative_command(NULL, 1, 4, args, NULL);
	relative_command(NULL, 1, 3, args, NULL);
	assert(calls == 1);
	puts("Relative mouse command registration, capability and argument checks passed");
	return (0);
}
