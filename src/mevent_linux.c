/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 */

/*
 * Linux implementation of the micro event interface.  The public mevent API
 * remains the same as the kqueue implementation; this file maps it to epoll,
 * eventfd, timerfd, and inotify without installing signal handlers.
 */

#include <support/freebsd_compat.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/queue.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "compat.h"
#include "mevent.h"

#define	MEVENT_MAX	64

#define	MEVENT_ADD		0x0001
#define	MEVENT_DISABLED		0x0002

static pthread_t mevent_tid;
static pthread_once_t mevent_once = PTHREAD_ONCE_INIT;
static int mevent_epfd = -1;
static int mevent_wakefd = -1;
static pthread_mutex_t mevent_lmutex = PTHREAD_MUTEX_INITIALIZER;

struct mevent {
	void (*me_func)(int, enum ev_type, void *);
	int me_fd;
	int me_poll_fd;
	int me_inotify_wd;
	enum ev_type me_type;
	void *me_param;
	int me_closefd;
	int me_fflags;
	bool me_enabled;
	bool me_deleted;
	bool me_registered;
	LIST_ENTRY(mevent) me_list;
};

static LIST_HEAD(listhead, mevent) global_head;

static void
mevent_qlock(void)
{
	pthread_mutex_lock(&mevent_lmutex);
}

static void
mevent_qunlock(void)
{
	pthread_mutex_unlock(&mevent_lmutex);
}

static uint32_t
mevent_epoll_events(enum ev_type type)
{
	switch (type) {
	case EVF_READ:
		return (EPOLLIN);
	case EVF_WRITE:
		return (EPOLLOUT);
	case EVF_TIMER:
	case EVF_VNODE:
	case EVF_SIGNAL:
		return (EPOLLIN);
	}

	return (0);
}

static int
mevent_ctl(struct mevent *mevp, int op)
{
	struct epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = mevent_epoll_events(mevp->me_type);
	ev.data.ptr = mevp;

	return (epoll_ctl(mevent_epfd, op, mevp->me_poll_fd, &ev));
}

static void
mevent_timer_set(struct mevent *mevp, bool armed)
{
	struct itimerspec ts;
	uint64_t nsec;

	assert(mevp->me_type == EVF_TIMER);

	memset(&ts, 0, sizeof(ts));
	if (armed) {
		nsec = (uint64_t)mevp->me_fd * 1000000;
		if (nsec == 0)
			nsec = 1;
		ts.it_value.tv_sec = nsec / 1000000000;
		ts.it_value.tv_nsec = nsec % 1000000000;
		ts.it_interval = ts.it_value;
	}

	(void)timerfd_settime(mevp->me_poll_fd, 0, &ts, NULL);
}

static void
mevent_notify(void)
{
	uint64_t v = 1;

	if (mevent_wakefd >= 0 && pthread_self() != mevent_tid)
		(void)write(mevent_wakefd, &v, sizeof(v));
}

static void
mevent_drain_wakefd(void)
{
	uint64_t v;

	for (;;) {
		if (read(mevent_wakefd, &v, sizeof(v)) == sizeof(v))
			continue;
		if (errno == EINTR)
			continue;
		return;
	}
}

static void
mevent_init(void)
{
	struct epoll_event ev;

	LIST_INIT(&global_head);

	mevent_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (mevent_epfd < 0) {
		perror("epoll_create1");
		abort();
	}

	mevent_wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (mevent_wakefd < 0) {
		perror("eventfd");
		abort();
	}

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = NULL;
	if (epoll_ctl(mevent_epfd, EPOLL_CTL_ADD, mevent_wakefd, &ev) != 0) {
		perror("epoll_ctl");
		abort();
	}
}

static bool
mevent_is_current(struct mevent *mevp)
{
	bool current;

	mevent_qlock();
	current = mevp->me_enabled && !mevp->me_deleted;
	mevent_qunlock();

	return (current);
}

static void
mevent_call(struct mevent *mevp)
{
	if (mevent_is_current(mevp))
		(*mevp->me_func)(mevp->me_fd, mevp->me_type, mevp->me_param);
}

static void
mevent_unregister_locked(struct mevent *mevp)
{
	if (!mevp->me_registered)
		return;

	(void)mevent_ctl(mevp, EPOLL_CTL_DEL);
	mevp->me_registered = false;
}

static int
mevent_register_locked(struct mevent *mevp)
{
	if (mevp->me_registered)
		return (0);

	if (mevent_ctl(mevp, EPOLL_CTL_ADD) != 0)
		return (errno);

	mevp->me_registered = true;
	return (0);
}

static void
mevent_close_event(struct mevent *mevp)
{
	if (mevp->me_type == EVF_TIMER || mevp->me_type == EVF_VNODE ||
	    mevp->me_type == EVF_SIGNAL) {
		if (mevp->me_poll_fd >= 0)
			close(mevp->me_poll_fd);
	}
	if (mevp->me_closefd && mevp->me_type != EVF_TIMER &&
	    mevp->me_type != EVF_SIGNAL)
		close(mevp->me_fd);
}

static void
mevent_cleanup_deleted(void)
{
	struct mevent *mevp, *tmp;

	mevent_qlock();
	LIST_FOREACH_SAFE(mevp, &global_head, me_list, tmp) {
		if (!mevp->me_deleted)
			continue;

		mevent_unregister_locked(mevp);
		LIST_REMOVE(mevp, me_list);
		mevent_close_event(mevp);
		free(mevp);
	}
	mevent_qunlock();
}

static int
mevent_signal_watch(struct mevent *mevp)
{
	sigset_t mask;
	int error;

	sigemptyset(&mask);
	if (sigaddset(&mask, mevp->me_fd) != 0)
		return (errno);

	error = pthread_sigmask(SIG_BLOCK, &mask, NULL);
	if (error != 0)
		return (error);

	mevp->me_poll_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (mevp->me_poll_fd < 0)
		return (errno);

	return (0);
}

static int
mevent_vnode_watch(struct mevent *mevp)
{
	char procpath[64];
	char path[PATH_MAX];
	ssize_t len;
	uint32_t mask;

	snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", mevp->me_fd);
	len = readlink(procpath, path, sizeof(path) - 1);
	if (len < 0)
		return (errno);
	path[len] = '\0';

	mevp->me_poll_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (mevp->me_poll_fd < 0)
		return (errno);

	mask = IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE_SELF |
	    IN_DELETE_SELF;
	mevp->me_inotify_wd = inotify_add_watch(mevp->me_poll_fd, path, mask);
	if (mevp->me_inotify_wd < 0) {
		int error = errno;
		close(mevp->me_poll_fd);
		mevp->me_poll_fd = -1;
		return (error);
	}

	return (0);
}

static struct mevent *
mevent_add_state(int tfd, enum ev_type type,
    void (*func)(int, enum ev_type, void *), void *param, int state, int fflags)
{
	struct mevent *lp, *mevp;
	int error;

	if (tfd < 0 || func == NULL)
		return (NULL);

	if (type != EVF_READ && type != EVF_WRITE && type != EVF_TIMER &&
	    type != EVF_SIGNAL && type != EVF_VNODE)
		return (NULL);

	pthread_once(&mevent_once, mevent_init);

	mevp = calloc(1, sizeof(struct mevent));
	if (mevp == NULL)
		return (NULL);

	mevp->me_fd = tfd;
	mevp->me_poll_fd = tfd;
	mevp->me_inotify_wd = -1;
	mevp->me_type = type;
	mevp->me_func = func;
	mevp->me_param = param;
	mevp->me_fflags = fflags;
	mevp->me_enabled = (state & MEVENT_DISABLED) == 0;

	if (type == EVF_TIMER) {
		mevp->me_poll_fd = timerfd_create(CLOCK_MONOTONIC,
		    TFD_NONBLOCK | TFD_CLOEXEC);
		if (mevp->me_poll_fd < 0) {
			free(mevp);
			return (NULL);
		}
		mevent_timer_set(mevp, mevp->me_enabled);
	} else if (type == EVF_SIGNAL) {
		error = mevent_signal_watch(mevp);
		if (error != 0) {
			free(mevp);
			errno = error;
			return (NULL);
		}
	} else if (type == EVF_VNODE) {
		error = mevent_vnode_watch(mevp);
		if (error != 0) {
			free(mevp);
			errno = error;
			return (NULL);
		}
	}

	mevent_qlock();
	LIST_FOREACH(lp, &global_head, me_list) {
		if (!lp->me_deleted && type != EVF_TIMER && lp->me_fd == tfd &&
		    lp->me_type == type) {
			mevent_qunlock();
			mevent_close_event(mevp);
			free(mevp);
			errno = EEXIST;
			return (NULL);
		}
	}

	error = 0;
	if (mevp->me_enabled)
		error = mevent_register_locked(mevp);
	if (error == 0)
		LIST_INSERT_HEAD(&global_head, mevp, me_list);
	mevent_qunlock();

	if (error != 0) {
		mevent_close_event(mevp);
		free(mevp);
		errno = error;
		return (NULL);
	}

	mevent_notify();
	return (mevp);
}

struct mevent *
mevent_add(int tfd, enum ev_type type, void (*func)(int, enum ev_type, void *),
    void *param)
{
	return (mevent_add_state(tfd, type, func, param, MEVENT_ADD, 0));
}

struct mevent *
mevent_add_flags(int tfd, enum ev_type type, int fflags,
    void (*func)(int, enum ev_type, void *), void *param)
{
	return (mevent_add_state(tfd, type, func, param, MEVENT_ADD, fflags));
}

struct mevent *
mevent_add_disabled(int tfd, enum ev_type type,
    void (*func)(int, enum ev_type, void *), void *param)
{
	return (mevent_add_state(tfd, type, func, param,
	    MEVENT_ADD | MEVENT_DISABLED, 0));
}

int
mevent_enable(struct mevent *evp)
{
	int error;

	if (evp == NULL)
		return (EINVAL);

	mevent_qlock();
	if (evp->me_deleted) {
		mevent_qunlock();
		return (EINVAL);
	}
	if (evp->me_type == EVF_TIMER)
		mevent_timer_set(evp, true);
	error = mevent_register_locked(evp);
	if (error == 0)
		evp->me_enabled = true;
	mevent_qunlock();

	mevent_notify();
	return (error);
}

int
mevent_disable(struct mevent *evp)
{
	if (evp == NULL)
		return (EINVAL);

	mevent_qlock();
	if (evp->me_deleted) {
		mevent_qunlock();
		return (EINVAL);
	}
	mevent_unregister_locked(evp);
	if (evp->me_type == EVF_TIMER)
		mevent_timer_set(evp, false);
	evp->me_enabled = false;
	mevent_qunlock();

	mevent_notify();
	return (0);
}

int
mevent_timer_update(struct mevent *evp, int msecs)
{
	if (evp == NULL || evp->me_type != EVF_TIMER || msecs < 0)
		return (EINVAL);

	mevent_qlock();
	if (evp->me_deleted) {
		mevent_qunlock();
		return (EINVAL);
	}
	evp->me_fd = msecs;
	if (evp->me_enabled)
		mevent_timer_set(evp, true);
	mevent_qunlock();

	mevent_notify();
	return (0);
}

static int
mevent_delete_event(struct mevent *evp, int closefd)
{
	if (evp == NULL)
		return (EINVAL);

	mevent_qlock();
	evp->me_deleted = true;
	evp->me_enabled = false;
	if (closefd)
		evp->me_closefd = 1;
	mevent_unregister_locked(evp);
	mevent_qunlock();

	mevent_notify();
	return (0);
}

int
mevent_delete(struct mevent *evp)
{
	return (mevent_delete_event(evp, 0));
}

int
mevent_delete_close(struct mevent *evp)
{
	return (mevent_delete_event(evp, 1));
}

static void
mevent_set_name(void)
{
	compat_set_thread_name(mevent_tid, "mevent");
}

static void
mevent_drain_timerfd(int fd)
{
	uint64_t expirations;

	for (;;) {
		if (read(fd, &expirations, sizeof(expirations)) ==
		    sizeof(expirations))
			continue;
		if (errno == EINTR)
			continue;
		return;
	}
}

static void
mevent_drain_signalfd(int fd)
{
	struct signalfd_siginfo si[MEVENT_MAX];

	for (;;) {
		if (read(fd, si, sizeof(si)) > 0)
			continue;
		if (errno == EINTR)
			continue;
		return;
	}
}

static void
mevent_drain_inotify(int fd)
{
	char buf[4096]
	    __attribute__((aligned(__alignof__(struct inotify_event))));

	for (;;) {
		if (read(fd, buf, sizeof(buf)) > 0)
			continue;
		if (errno == EINTR)
			continue;
		return;
	}
}

static void
mevent_handle_event(struct mevent *mevp, uint32_t events)
{
	if ((events & (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP)) == 0)
		return;

	switch (mevp->me_type) {
	case EVF_TIMER:
		mevent_drain_timerfd(mevp->me_poll_fd);
		break;
	case EVF_VNODE:
		mevent_drain_inotify(mevp->me_poll_fd);
		break;
	case EVF_SIGNAL:
		mevent_drain_signalfd(mevp->me_poll_fd);
		break;
	case EVF_READ:
	case EVF_WRITE:
		break;
	}

	mevent_call(mevp);
}

void
mevent_dispatch(void)
{
	struct epoll_event events[MEVENT_MAX];
	int i, num_events;

	mevent_tid = pthread_self();
	mevent_set_name();

	pthread_once(&mevent_once, mevent_init);

	for (;;) {
		mevent_cleanup_deleted();

		num_events = epoll_wait(mevent_epfd, events, MEVENT_MAX, -1);
		if (num_events < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			continue;
		}

		for (i = 0; i < num_events; i++) {
			if (events[i].data.ptr == NULL) {
				mevent_drain_wakefd();
				continue;
			}
			mevent_handle_event(events[i].data.ptr, events[i].events);
		}
	}
}
