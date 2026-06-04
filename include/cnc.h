/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
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

#pragma once

#include <sys/queue.h>
#include <sys/socket.h>

typedef struct cnc_conn_t* cnc_conn_t;

typedef void (*CMD_HANDLER)(cnc_conn_t conn,  int req_id, int argc, char *argv[], void *param);

struct cnc_command {
    const char* cmd;
    CMD_HANDLER cmd_handler;

    const void* param;

    LIST_ENTRY(cnc_command) entries; 
};

void  cnc_register_command(const char *cmd, CMD_HANDLER handler, void *param);
void  cnc_unregister_commands_by_param(void *param);
int   cnc_start_srv();
int   cnc_connect_client(const char *socket_path);

void cnc_send_response(struct cnc_conn_t *c, int response_id, const char *data);
void cnc_send_notification(const char *data);
void cnc_send_notification_to(struct cnc_conn_t *c, const char *data);
