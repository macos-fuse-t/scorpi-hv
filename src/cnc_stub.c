/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include "cnc.h"

void
cnc_register_command(const char *cmd, CMD_HANDLER handler, void *param)
{
	(void)cmd;
	(void)handler;
	(void)param;
}

void
cnc_unregister_commands_by_param(void *param)
{
	(void)param;
}

int
cnc_start_srv(void)
{
	return (0);
}

void
cnc_send_response(struct cnc_conn_t *c, int response_id, const char *data)
{
	(void)c;
	(void)response_id;
	(void)data;
}

void
cnc_send_notification(const char *data)
{
	(void)data;
}

void
cnc_send_notification_to(struct cnc_conn_t *c, const char *data)
{
	(void)c;
	(void)data;
}
