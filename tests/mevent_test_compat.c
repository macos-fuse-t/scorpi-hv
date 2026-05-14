#include <pthread.h>

int
compat_set_thread_name(pthread_t thread, const char *name)
{
	(void)thread;
	(void)name;
	return (0);
}
