/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Hans Rosenfeld
 * Author: Hans Rosenfeld <rosenfeld@grumpf.hope-2000.org>
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <support/endian.h>

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "tpm_emul.h"

struct tpm_swtpm {
	int fd;
};

struct tpm_resp_hdr {
	uint16_t tag;
	uint32_t len;
	uint32_t errcode;
} __attribute__((packed));

static int
tpm_swtpm_read_full(int fd, void *buf, size_t len)
{
	uint8_t *p;

	p = buf;
	while (len > 0) {
		ssize_t done;

		done = recv(fd, p, len, 0);
		if (done == 0)
			return (ECONNRESET);
		if (done < 0) {
			if (errno == EINTR)
				continue;
			return (errno);
		}

		p += done;
		len -= done;
	}

	return (0);
}

static int
tpm_swtpm_write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *p;

	p = buf;
	while (len > 0) {
		ssize_t done;

#ifdef MSG_NOSIGNAL
		done = send(fd, p, len, MSG_NOSIGNAL);
#else
		done = send(fd, p, len, 0);
#endif
		if (done < 0) {
			if (errno == EINTR)
				continue;
			return (errno);
		}

		p += done;
		len -= done;
	}

	return (0);
}

static int
tpm_swtpm_init(void **sc, nvlist_t *nvl)
{
	struct sockaddr_un tpm_addr;
	struct tpm_swtpm *tpm;
	const char *path;
	int error;

	tpm = calloc(1, sizeof(*tpm));
	if (tpm == NULL)
		return (ENOMEM);
	tpm->fd = -1;

	path = get_config_value_node(nvl, "path");
	if (path == NULL) {
		warnx("%s: no socket path specified", __func__);
		error = ENOENT;
		goto err_out;
	}

	tpm->fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (tpm->fd < 0) {
		error = errno;
		warnc(error, "%s: unable to open tpm socket", __func__);
		goto err_out;
	}

#ifdef SO_NOSIGPIPE
	{
		int on = 1;

		(void)setsockopt(tpm->fd, SOL_SOCKET, SO_NOSIGPIPE, &on,
		    sizeof(on));
	}
#endif

	memset(&tpm_addr, 0, sizeof(tpm_addr));
	tpm_addr.sun_family = AF_UNIX;
	if (snprintf(tpm_addr.sun_path, sizeof(tpm_addr.sun_path), "%s", path) >=
	    (int)sizeof(tpm_addr.sun_path)) {
		warnx("%s: socket path too long: %s", __func__, path);
		error = ENAMETOOLONG;
		goto err_out;
	}

	if (connect(tpm->fd, (struct sockaddr *)&tpm_addr, sizeof(tpm_addr)) !=
	    0) {
		error = errno;
		warnc(error, "%s: unable to connect to tpm socket \"%s\"",
		    __func__, path);
		goto err_out;
	}
	*sc = tpm;

	return (0);

err_out:
	if (tpm->fd >= 0)
		close(tpm->fd);
	free(tpm);

	return (error);
}

static int
tpm_swtpm_execute_cmd(void *sc, void *cmd, uint32_t cmd_size, void *rsp,
    uint32_t rsp_size)
{
	struct tpm_resp_hdr hdr;
	struct tpm_swtpm *tpm;
	uint32_t rsp_len;
	int error;

	if (rsp_size < sizeof(hdr)) {
		warnx("%s: response buffer is too small", __func__);
		return (EINVAL);
	}

	tpm = sc;

	error = tpm_swtpm_write_full(tpm->fd, cmd, cmd_size);
	if (error != 0) {
		warnc(error, "%s: command send failed", __func__);
		return (error);
	}

	error = tpm_swtpm_read_full(tpm->fd, &hdr, sizeof(hdr));
	if (error != 0) {
		warnc(error, "%s: response header read failed", __func__);
		return (error);
	}

	rsp_len = be32toh(hdr.len);
	if (rsp_len < sizeof(hdr)) {
		warnx("%s: invalid response length %u", __func__, rsp_len);
		return (EFAULT);
	}
	if (rsp_len > rsp_size) {
		warnx("%s: response length %u exceeds buffer %u", __func__,
		    rsp_len, rsp_size);
		return (EOVERFLOW);
	}

	memcpy(rsp, &hdr, sizeof(hdr));
	error = tpm_swtpm_read_full(tpm->fd, (uint8_t *)rsp + sizeof(hdr),
	    rsp_len - sizeof(hdr));
	if (error != 0) {
		warnc(error, "%s: response body read failed", __func__);
		return (error);
	}

	return (0);
}

static void
tpm_swtpm_deinit(void *sc)
{
	struct tpm_swtpm *tpm;

	tpm = sc;
	if (tpm == NULL)
		return;

	if (tpm->fd >= 0)
		close(tpm->fd);

	free(tpm);
}

static const struct tpm_emul tpm_emul_swtpm = {
	.name = "swtpm",
	.init = tpm_swtpm_init,
	.deinit = tpm_swtpm_deinit,
	.execute_cmd = tpm_swtpm_execute_cmd,
};
TPM_EMUL_SET(tpm_emul_swtpm);
