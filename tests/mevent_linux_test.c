#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <support/freebsd_compat.h>
#include "mevent.h"

struct test_state {
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	int pipe_rfd;
	int pipe_wfd;
	int file_fd;
	int read_count;
	int timer_count;
	int vnode_count;
	int signal_count;
	int disabled_count;
};

static void
fail(const char *msg)
{
	perror(msg);
	exit(1);
}

static void
lock_state(struct test_state *state)
{
	if (pthread_mutex_lock(&state->mtx) != 0)
		fail("pthread_mutex_lock");
}

static void
unlock_state(struct test_state *state)
{
	if (pthread_mutex_unlock(&state->mtx) != 0)
		fail("pthread_mutex_unlock");
}

static void
notify_state(struct test_state *state)
{
	if (pthread_cond_broadcast(&state->cond) != 0)
		fail("pthread_cond_broadcast");
}

static void
pipe_cb(int fd, enum ev_type type, void *arg)
{
	struct test_state *state = arg;
	char buf[16];

	if (type != EVF_READ)
		exit(2);
	while (read(fd, buf, sizeof(buf)) > 0)
		;

	lock_state(state);
	state->read_count++;
	notify_state(state);
	unlock_state(state);
}

static void
timer_cb(int fd __unused, enum ev_type type, void *arg)
{
	struct test_state *state = arg;

	if (type != EVF_TIMER)
		exit(2);

	lock_state(state);
	state->timer_count++;
	notify_state(state);
	unlock_state(state);
}

static void
vnode_cb(int fd __unused, enum ev_type type, void *arg)
{
	struct test_state *state = arg;

	if (type != EVF_VNODE)
		exit(2);

	lock_state(state);
	state->vnode_count++;
	notify_state(state);
	unlock_state(state);
}

static void
signal_cb(int fd __unused, enum ev_type type, void *arg)
{
	struct test_state *state = arg;

	if (type != EVF_SIGNAL)
		exit(2);

	lock_state(state);
	state->signal_count++;
	notify_state(state);
	unlock_state(state);
}

static void
disabled_cb(int fd __unused, enum ev_type type __unused, void *arg)
{
	struct test_state *state = arg;

	lock_state(state);
	state->disabled_count++;
	notify_state(state);
	unlock_state(state);
}

static void *
dispatch_thread(void *arg __unused)
{
	mevent_dispatch();
	return (NULL);
}

static bool
wait_for_count(struct test_state *state, int *counter, int minimum)
{
	struct timespec deadline;
	int error;

	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
		fail("clock_gettime");
	deadline.tv_sec += 2;

	lock_state(state);
	while (*counter < minimum) {
		error = pthread_cond_timedwait(&state->cond, &state->mtx,
		    &deadline);
		if (error == ETIMEDOUT)
			break;
		if (error != 0) {
			errno = error;
			fail("pthread_cond_timedwait");
		}
	}
	error = *counter >= minimum;
	unlock_state(state);

	return (error);
}

static int
set_nonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return (-1);
	return (fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

int
main(void)
{
	struct test_state state;
	struct mevent *read_ev, *timer_ev, *vnode_ev, *signal_ev, *disabled_ev;
	pthread_t tid;
	sigset_t sigmask;
	char template[] = "/tmp/scorpi-mevent-test.XXXXXX";
	int pipefd[2];
	int error;

	memset(&state, 0, sizeof(state));
	if (pthread_mutex_init(&state.mtx, NULL) != 0)
		fail("pthread_mutex_init");
	if (pthread_cond_init(&state.cond, NULL) != 0)
		fail("pthread_cond_init");

	if (pipe(pipefd) != 0)
		fail("pipe");
	if (set_nonblock(pipefd[0]) != 0)
		fail("fcntl pipe read");
	if (set_nonblock(pipefd[1]) != 0)
		fail("fcntl pipe write");
	state.pipe_rfd = pipefd[0];
	state.pipe_wfd = pipefd[1];

	state.file_fd = mkstemp(template);
	if (state.file_fd < 0)
		fail("mkstemp");

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	error = pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
	if (error != 0) {
		errno = error;
		fail("pthread_sigmask block");
	}

	if (pthread_create(&tid, NULL, dispatch_thread, NULL) != 0)
		fail("pthread_create");
	pthread_detach(tid);

	read_ev = mevent_add(state.pipe_rfd, EVF_READ, pipe_cb, &state);
	if (read_ev == NULL)
		fail("mevent_add read");

	timer_ev = mevent_add(20, EVF_TIMER, timer_cb, &state);
	if (timer_ev == NULL)
		fail("mevent_add timer");

	vnode_ev = mevent_add_flags(state.file_fd, EVF_VNODE, EVFF_ATTRIB,
	    vnode_cb, &state);
	if (vnode_ev == NULL)
		fail("mevent_add vnode");

	signal_ev = mevent_add(SIGUSR1, EVF_SIGNAL, signal_cb, &state);
	if (signal_ev == NULL)
		fail("mevent_add signal");

	disabled_ev = mevent_add_disabled(state.pipe_rfd, EVF_READ,
	    disabled_cb, &state);
	if (disabled_ev != NULL) {
		fprintf(stderr, "duplicate disabled read event unexpectedly added\n");
		return (1);
	}

	if (write(state.pipe_wfd, "x", 1) != 1)
		fail("write pipe");
	if (!wait_for_count(&state, &state.read_count, 1)) {
		fprintf(stderr, "read event did not fire\n");
		return (1);
	}

	if (!wait_for_count(&state, &state.timer_count, 1)) {
		fprintf(stderr, "timer event did not fire\n");
		return (1);
	}

	if (fchmod(state.file_fd, 0644) != 0)
		fail("fchmod");
	if (!wait_for_count(&state, &state.vnode_count, 1)) {
		fprintf(stderr, "vnode event did not fire\n");
		return (1);
	}

	if (kill(getpid(), SIGUSR1) != 0)
		fail("kill");
	if (!wait_for_count(&state, &state.signal_count, 1)) {
		fprintf(stderr, "signal event did not fire\n");
		return (1);
	}

	if (mevent_disable(read_ev) != 0)
		fail("mevent_disable");
	lock_state(&state);
	state.read_count = 0;
	unlock_state(&state);
	if (write(state.pipe_wfd, "y", 1) != 1)
		fail("write pipe disabled");
	if (wait_for_count(&state, &state.read_count, 1)) {
		fprintf(stderr, "disabled read event fired\n");
		return (1);
	}

	if (mevent_enable(read_ev) != 0)
		fail("mevent_enable");
	if (write(state.pipe_wfd, "z", 1) != 1)
		fail("write pipe enabled");
	if (!wait_for_count(&state, &state.read_count, 1)) {
		fprintf(stderr, "reenabled read event did not fire\n");
		return (1);
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	error = pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);
	if (error != 0) {
		errno = error;
		fail("pthread_sigmask unblock");
	}

	mevent_delete(read_ev);
	mevent_delete(timer_ev);
	mevent_delete(vnode_ev);
	mevent_delete(signal_ev);
	close(state.pipe_rfd);
	close(state.pipe_wfd);
	close(state.file_fd);
	(void)unlink(template);

	return (0);
}
