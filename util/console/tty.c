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

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "websock_client.h"

struct termios orig_termios;
int sockfd;
int ctrl_a_pressed = 0;
int virtio_console = 0;
int detach_seq_match = 0;

static const char detach_seq[] = "\033]scorpi-detach\007";

struct resize_message {
	int rows;
	int cols;
};

static void
restore_terminal()
{
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
	printf("\033[?25h\033[?7h\033[r\033c"); // Reset terminal, enable
						// scrolling and wrapping
	fflush(stdout);
}

static void
set_raw_mode()
{
	struct termios raw;
	tcgetattr(STDIN_FILENO, &orig_termios);
	atexit(restore_terminal);

	raw = orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | ISIG);
	raw.c_iflag &= ~(IXON);
	raw.c_iflag |= ICRNL;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);

	printf("\033[?7h\033[?25h"); // Enable line wrapping and show cursor
	fflush(stdout);
	system("stty -ixon"); // Disable flow control (Ctrl+S/Ctrl+Q issues)
}

static void
handle_resize(int sig)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
		return;

	if (virtio_console)
		vm_resize_request(1, ws.ws_row, ws.ws_col);
}

static void
handle_signal(int sig)
{
	if (sig == SIGINT || sig == SIGTERM) {
		restore_terminal();
		close(sockfd);
		exit(0);
	}
}

static void
detach_console()
{
	restore_terminal();
	close(sockfd);
	exit(0);
}

static int
write_console_output(const char *buffer, int n)
{
	char out[1024];
	int out_len = 0;
	size_t detach_len = strlen(detach_seq);

	for (int i = 0; i < n; i++) {
		char c = buffer[i];

		if (c == detach_seq[detach_seq_match]) {
			detach_seq_match++;
			if ((size_t)detach_seq_match == detach_len)
				return (1);
			continue;
		}
		if (detach_seq_match > 0) {
			if (out_len + detach_seq_match > (int)sizeof(out)) {
				write(STDOUT_FILENO, out, out_len);
				out_len = 0;
			}
			memcpy(out + out_len, detach_seq, detach_seq_match);
			out_len += detach_seq_match;
			detach_seq_match = 0;
			if (c == detach_seq[0]) {
				detach_seq_match = 1;
				continue;
			}
		}
		if (out_len == (int)sizeof(out)) {
			write(STDOUT_FILENO, out, out_len);
			out_len = 0;
		}
		out[out_len++] = c;
	}
	if (out_len > 0)
		write(STDOUT_FILENO, out, out_len);
	return (0);
}

static void
usage(const char *argv)
{
	fprintf(stderr,
	    "Usage: %s [-l vm_control_sock] tty_sock, where tyy_sock is "
	    "<[server_ip]:port> or <unix_socket_path>\n",
	    argv);
	exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
	int opt;
	char *sock_path = NULL;
	char *tty_path = NULL;

	while ((opt = getopt(argc, argv, "l:")) != -1) {
		switch (opt) {
		case 'l':
			sock_path = optarg;
			virtio_console = 1;
			break;
		case '?':
			usage(argv[0]);
		}
	}

	if (optind >= argc)
		usage(argv[0]);

	tty_path = argv[optind];
	if (virtio_console) {
		if (vm_connect(sock_path) != 0) {
			fprintf(stderr, "failed to connect to vm\n");
			exit(-1);
		}
		handle_resize(0);
	}

	char *input = tty_path;
	char *colon = strrchr(input, ':');
	struct sockaddr_un server_addr_un;
	struct sockaddr_in server_addr_in;
	char buffer[1024];
	fd_set read_fds;

	if (colon) { // TCP connection
		*colon = '\0';
		const char *server_ip = (*input == '\0') ? "127.0.0.1" : input;
		int server_port = atoi(colon + 1);

		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) {
			perror("socket");
			exit(EXIT_FAILURE);
		}

		server_addr_in.sin_family = AF_INET;
		server_addr_in.sin_port = htons(server_port);
		inet_pton(AF_INET, server_ip, &server_addr_in.sin_addr);

		if (connect(sockfd, (struct sockaddr *)&server_addr_in,
			sizeof(server_addr_in)) < 0) {
			perror("connect");
			exit(EXIT_FAILURE);
		}
	} else { // Unix socket connection
		sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sockfd < 0) {
			perror("socket");
			exit(EXIT_FAILURE);
		}

		memset(&server_addr_un, 0, sizeof(server_addr_un));
		server_addr_un.sun_family = AF_UNIX;
		strncpy(server_addr_un.sun_path, input,
		    sizeof(server_addr_un.sun_path) - 1);

		if (connect(sockfd, (struct sockaddr *)&server_addr_un,
			sizeof(server_addr_un)) < 0) {
			perror("connect");
			exit(EXIT_FAILURE);
		}
	}

	set_raw_mode();
	signal(SIGWINCH, handle_resize);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	fprintf(stderr,
	    "Connected to %s. press CTRL+A, then CTRL+D to detach (like screen).\n",
	    input);

	while (1) {
		FD_ZERO(&read_fds);
		FD_SET(STDIN_FILENO, &read_fds);
		FD_SET(sockfd, &read_fds);

		int ret = select(sockfd + 1, &read_fds, NULL, NULL, NULL);
		if (ret < 0) {
			if (errno == EINTR) {
				continue; // Restart select if interrupted
			}
			perror("select");
			break;
		}

		if (FD_ISSET(STDIN_FILENO, &read_fds)) {
			int n = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
			if (n > 0) {
				buffer[n] = '\0';
				if (buffer[0] == 1) { // CTRL+A pressed
					ctrl_a_pressed = 1;
				} else if (ctrl_a_pressed &&
				    buffer[0] == 4) { // CTRL+D after CTRL+A
					printf("Detaching...\n");
					restore_terminal();
					close(sockfd);
					exit(0);
				} else {
					ctrl_a_pressed = 0;
				}
				write(sockfd, buffer, n);
			}
		}
		if (FD_ISSET(sockfd, &read_fds)) {
			int n = read(sockfd, buffer, sizeof(buffer));
			if (n > 0) {
				// Replace all occurrences of \033[?7l with
				// \033[?7h to enforce line wrapping
				const char esc_disable[] = "\x1B[?7l";
				const char esc_enable[] = "\x1B[?7h";
				const int esc_len =
				    5; // Length of the escape sequence

				for (int i = 0; i <= n - esc_len; i++) {
					if (memcmp(buffer + i, esc_disable,
						esc_len) == 0) {
						memcpy(buffer + i, esc_enable,
						    esc_len);
						i += esc_len -
						    1; // Skip ahead to avoid
						       // overlapping matches
					}
				}
				if (write_console_output(buffer, n))
					detach_console();
			} else {
				printf("Disconnected.\n");
				break;
			}
		}
	}

	restore_terminal();
	close(sockfd);
	return 0;
}
