#pragma once

#include <stddef.h>
#include <sys/types.h>

#include <scorpi/protocol/vhost_user.h>

#define VHOST_USER_MAX_FDS 8

int vhost_user_connect(const char *path);
int vhost_user_listen(const char *path, int backlog);
int vhost_user_accept(int listen_fd);
int vhost_user_make_notify_fds(int fds[2]);
void vhost_user_msg_init(struct scorpi_vhost_msg *msg, uint32_t request,
    uint32_t payload_size);
ssize_t vhost_user_send_msg(int fd, const struct scorpi_vhost_msg *msg,
    const int *fds, size_t fd_count);
ssize_t vhost_user_recv_msg(int fd, struct scorpi_vhost_msg *msg, int *fds,
    size_t max_fds, size_t *fd_count);
ssize_t vhost_user_send_fds(int fd, const void *buf, size_t len,
    const int *fds, size_t fd_count);
ssize_t vhost_user_recv_fds(int fd, void *buf, size_t len, int *fds,
    size_t max_fds, size_t *fd_count);
