#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "vhost_user.h"

#ifndef SUN_LEN
#define SUN_LEN(su) (sizeof(*(su)))
#endif

static int
vhost_user_sockaddr(const char *path, struct sockaddr_un *sun, socklen_t *len)
{
	size_t path_len;

	if (path == NULL || path[0] == '\0') {
		errno = EINVAL;
		return (-1);
	}
	path_len = strlen(path);
	if (path_len >= sizeof(sun->sun_path)) {
		errno = ENAMETOOLONG;
		return (-1);
	}

	memset(sun, 0, sizeof(*sun));
	sun->sun_family = AF_UNIX;
	memcpy(sun->sun_path, path, path_len + 1);
	*len = (socklen_t)SUN_LEN(sun);
	return (0);
}

int
vhost_user_connect(const char *path)
{
	struct sockaddr_un sun;
	socklen_t len;
	int fd;

	if (vhost_user_sockaddr(path, &sun, &len) != 0)
		return (-1);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return (-1);
	if (connect(fd, (struct sockaddr *)&sun, len) != 0) {
		close(fd);
		return (-1);
	}
	return (fd);
}

int
vhost_user_listen(const char *path, int backlog)
{
	struct sockaddr_un sun;
	socklen_t len;
	int fd;

	if (vhost_user_sockaddr(path, &sun, &len) != 0)
		return (-1);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return (-1);
	if (unlink(path) != 0 && errno != ENOENT) {
		close(fd);
		return (-1);
	}
	if (bind(fd, (struct sockaddr *)&sun, len) != 0) {
		close(fd);
		return (-1);
	}
	if (listen(fd, backlog) != 0) {
		close(fd);
		unlink(path);
		return (-1);
	}
	return (fd);
}

int
vhost_user_accept(int listen_fd)
{
	int fd;

	do {
		fd = accept(listen_fd, NULL, NULL);
	} while (fd < 0 && errno == EINTR);
	return (fd);
}

int
vhost_user_make_notify_fds(int fds[2])
{
	if (fds == NULL) {
		errno = EINVAL;
		return (-1);
	}
	return (pipe(fds));
}

void
vhost_user_msg_init(struct scorpi_vhost_msg *msg, uint32_t request,
    uint32_t payload_size)
{
	memset(msg, 0, sizeof(*msg));
	msg->header.magic = SCORPI_VHOST_MAGIC;
	msg->header.version = SCORPI_VHOST_VERSION;
	msg->header.header_size = SCORPI_VHOST_HEADER_SIZE;
	msg->header.request = request;
	msg->header.size = payload_size;
}

ssize_t
vhost_user_send_msg(int fd, const struct scorpi_vhost_msg *msg,
    const int *fds, size_t fd_count)
{
	size_t len;

	if (msg == NULL ||
	    msg->header.header_size != SCORPI_VHOST_HEADER_SIZE ||
	    msg->header.size > sizeof(msg->payload)) {
		errno = EINVAL;
		return (-1);
	}
	len = SCORPI_VHOST_HEADER_SIZE + msg->header.size;
	return (vhost_user_send_fds(fd, msg, len, fds, fd_count));
}

ssize_t
vhost_user_recv_msg(int fd, struct scorpi_vhost_msg *msg, int *fds,
    size_t max_fds, size_t *fd_count)
{
	uint8_t *bytes;
	size_t expected;
	size_t have;
	ssize_t rc;

	if (msg == NULL) {
		errno = EINVAL;
		return (-1);
	}
	memset(msg, 0, sizeof(*msg));
	rc = vhost_user_recv_fds(fd, msg, sizeof(*msg), fds, max_fds,
	    fd_count);
	if (rc <= 0)
		return (rc);
	if ((size_t)rc < SCORPI_VHOST_HEADER_SIZE) {
		errno = EPROTO;
		return (-1);
	}
	if (msg->header.magic != SCORPI_VHOST_MAGIC ||
	    msg->header.version != SCORPI_VHOST_VERSION ||
	    msg->header.header_size != SCORPI_VHOST_HEADER_SIZE ||
	    msg->header.size > sizeof(msg->payload)) {
		errno = EPROTO;
		return (-1);
	}

	expected = SCORPI_VHOST_HEADER_SIZE + msg->header.size;
	have = (size_t)rc;
	bytes = (uint8_t *)msg;
	while (have < expected) {
		rc = read(fd, bytes + have, expected - have);
		if (rc == 0) {
			errno = ECONNRESET;
			return (-1);
		}
		if (rc < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return (-1);
		}
		have += (size_t)rc;
	}
	return ((ssize_t)have);
}

ssize_t
vhost_user_send_fds(int fd, const void *buf, size_t len, const int *fds,
    size_t fd_count)
{
	char cmsgbuf[CMSG_SPACE(sizeof(int) * VHOST_USER_MAX_FDS)];
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	const uint8_t *bytes;
	size_t off;
	ssize_t rc;

	if (fd_count > VHOST_USER_MAX_FDS || (fd_count != 0 && fds == NULL)) {
		errno = EINVAL;
		return (-1);
	}

	bytes = buf;
	off = 0;
	while (off < len) {
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = (void *)(bytes + off);
		iov.iov_len = len - off;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		if (off == 0 && fd_count != 0) {
			memset(cmsgbuf, 0, sizeof(cmsgbuf));
			msg.msg_control = cmsgbuf;
			msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
			cmsg = CMSG_FIRSTHDR(&msg);
			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_RIGHTS;
			cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
			memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
		}

		do {
			rc = sendmsg(fd, &msg, 0);
		} while (rc < 0 && errno == EINTR);
		if (rc < 0)
			return (-1);
		if (rc == 0) {
			errno = EPIPE;
			return (-1);
		}
		off += (size_t)rc;
	}
	return ((ssize_t)off);
}

ssize_t
vhost_user_recv_fds(int fd, void *buf, size_t len, int *fds, size_t max_fds,
    size_t *fd_count)
{
	char cmsgbuf[CMSG_SPACE(sizeof(int) * VHOST_USER_MAX_FDS)];
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	size_t count;
	ssize_t rc;

	if (max_fds > VHOST_USER_MAX_FDS || (max_fds != 0 && fds == NULL)) {
		errno = EINVAL;
		return (-1);
	}

	memset(&msg, 0, sizeof(msg));
	memset(cmsgbuf, 0, sizeof(cmsgbuf));
	iov.iov_base = buf;
	iov.iov_len = len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	do {
		rc = recvmsg(fd, &msg, 0);
	} while (rc < 0 && errno == EINTR);
	if (rc < 0)
		return (-1);
	if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
		errno = EMSGSIZE;
		return (-1);
	}

	count = 0;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
	    cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		size_t bytes;
		size_t got;

		if (cmsg->cmsg_level != SOL_SOCKET ||
		    cmsg->cmsg_type != SCM_RIGHTS)
			continue;
		if (cmsg->cmsg_len < CMSG_LEN(0))
			continue;
		bytes = cmsg->cmsg_len - CMSG_LEN(0);
		got = bytes / sizeof(int);
		for (size_t i = 0; i < got; i++) {
			const uint8_t *fd_bytes;
			int received_fd;

			fd_bytes = (const uint8_t *)CMSG_DATA(cmsg);
			memcpy(&received_fd, fd_bytes + i * sizeof(received_fd),
			    sizeof(received_fd));
			if (count < max_fds) {
				fds[count++] = received_fd;
			} else {
				close(received_fd);
			}
		}
	}
	if (fd_count != NULL)
		*fd_count = count;
	return (rc);
}
