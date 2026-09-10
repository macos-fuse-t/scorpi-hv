/* SPDX-License-Identifier: BSD-2-Clause */
/* Assert LWS thread ownership while producers race session close/reopen. */
#include <assert.h>
#include <stdatomic.h>
#include <sched.h>

#define lws_get_protocol test_get_protocol
#define lws_callback_on_writable test_on_writable
#define lws_cancel_service test_cancel_service
#define lws_write test_write
#include "../src/cnc.c"

struct test_connection {
	const struct lws_protocols *protocol;
	bool writable;
	unsigned writes;
};

static pthread_t service_tid;
static atomic_uint wakeups, finished;
int raw_stdio;

const char *
get_config_value(const char *path)
{
	(void)path;
	return (NULL);
}

const struct lws_protocols *
test_get_protocol(struct lws *wsi)
{
	assert(pthread_equal(pthread_self(), service_tid));
	return (((struct test_connection *)wsi)->protocol);
}

int
test_on_writable(struct lws *wsi)
{
	assert(pthread_equal(pthread_self(), service_tid));
	((struct test_connection *)wsi)->writable = true;
	return (1);
}

void
test_cancel_service(struct lws_context *context)
{
	assert(context == global_context);
	atomic_fetch_add(&wakeups, 1);
}

int
test_write(struct lws *wsi, unsigned char *buf, size_t len,
    enum lws_write_protocol protocol)
{
	assert(pthread_equal(pthread_self(), service_tid));
	assert(protocol == LWS_WRITE_TEXT);
	assert(len > 2 && buf[0] == '{' && buf[len - 1] == '}');
	((struct test_connection *)wsi)->writes++;
	return ((int)len);
}

static void *
produce(void *arg)
{
	(void)arg;
	for (unsigned i = 0; i < 1000; i++) {
		cnc_send_notification("{\"test\":true}");
		sched_yield();
	}
	atomic_fetch_add(&finished, 1);
	return (NULL);
}

static void *
respond(void *arg)
{
	cnc_send_response(arg, 7, "{}");
	return (NULL);
}

static void
service(struct test_connection *conn, struct per_session_data *pss)
{
	/* LWS broadcasts cancellation without per-session user data. */
	cnc_callback((struct lws *)conn, LWS_CALLBACK_EVENT_WAIT_CANCELLED,
	    NULL, NULL, 0);
	while (conn->writable) {
		conn->writable = false;
		cnc_callback((struct lws *)conn, LWS_CALLBACK_SERVER_WRITEABLE,
		    pss, NULL, 0);
	}
}

int
main(void)
{
	struct test_connection control = { .protocol = &protocols[0] };
	struct test_connection term = { .protocol = &protocols[1] };
	struct per_session_data cp = { 0 }, tp = { 0 };
	pthread_t producers[4], responder;

	service_tid = pthread_self();
	/* Only our cancellation stub uses this sentinel. */
	global_context = (struct lws_context *)&control;
	service(&control, &cp); /* cancellation before any connections */
	cnc_init_session(&cp, (struct lws *)&control);
	cnc_init_session(&tp, (struct lws *)&term);

	assert(!pthread_create(&producers[0], NULL, produce, NULL));
	assert(!pthread_join(producers[0], NULL));
	assert(atomic_load(&wakeups) > 0);
	assert(!control.writable && !term.writable);
	assert(cp.queue_size == MAX_PENDING_MESSAGES && tp.queue_size == 0);
	service(&term, &tp);
	assert(!control.writable); /* protocol-specific cancellation */
	service(&control, &cp);
	assert(control.writes == MAX_PENDING_MESSAGES);

	/* A response to the terminal protocol also gets a service-thread wake. */
	assert(!pthread_create(&responder, NULL, respond, &tp));
	assert(!pthread_join(responder, NULL));
	assert(!term.writable);
	service(&term, &tp);
	assert(term.writes == 1);

	atomic_store(&finished, 0);
	for (unsigned i = 0; i < 4; i++)
		assert(!pthread_create(&producers[i], NULL, produce, NULL));
	while (atomic_load(&finished) != 4) {
		service(&control, &cp);
		cnc_clean_context(&cp);
		cnc_init_session(&cp, (struct lws *)&control);
		sched_yield();
	}
	for (unsigned i = 0; i < 4; i++)
		assert(!pthread_join(producers[i], NULL));
	service(&control, &cp);
	assert(term.writes == 1 && tp.queue_size == 0);
	cnc_clean_context(&cp);
	cnc_clean_context(&tp);
	service(&control, &cp); /* queued wake after all sessions closed */
	assert(LIST_EMPTY(&sessions));
	puts("cnc thread ownership, delivery, filtering and close race: PASS");
	return (0);
}
