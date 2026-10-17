#include "thread/thread.h"
#include "ntk_errno.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct ntk_thread {
    pthread_t native;
    ntk_thread_fn fn;
    void *user;
    ntk_thread_priority_t priority;
};

static void *thread_trampoline(void *arg)
{
    ntk_thread *t = (ntk_thread *)arg;
    ntk_thread_set_current_priority(t->priority);
    t->fn(t->user);
    return NULL;
}

ntk_thread *ntk_thread_create_ex(ntk_thread_fn fn, void *user, ntk_thread_priority_t priority)
{
    if (!fn)
        return NULL;
    ntk_thread *t = (ntk_thread *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fn = fn;
    t->user = user;
    t->priority = priority;
    if (pthread_create(&t->native, NULL, thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

ntk_thread *ntk_thread_create(ntk_thread_fn fn, void *user)
{
    return ntk_thread_create_ex(fn, user, NTK_THREAD_PRIO_NORMAL);
}

ntk_err_t ntk_thread_join(ntk_thread *t)
{
    if (!t)
        return NTK_ERR_INVALID;
    void *ret = NULL;
    if (pthread_join(t->native, &ret) != 0)
        return NTK_ERR_PLATFORM;
    return NTK_OK;
}

void ntk_thread_destroy(ntk_thread *t)
{
    free(t);
}

unsigned int ntk_thread_hardware_concurrency(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;
    return (unsigned int)n;
}
