#include "ztk/thread/thread.h"
#include "ztk/ztk_errno.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct ztk_thread {
    pthread_t native;
    ztk_thread_fn fn;
    void *user;
    ztk_thread_priority_t priority;
};

static void *thread_trampoline(void *arg)
{
    ztk_thread *t = (ztk_thread *)arg;
    ztk_thread_set_current_priority(t->priority);
    t->fn(t->user);
    return NULL;
}

ztk_thread *ztk_thread_create_ex(ztk_thread_fn fn, void *user, ztk_thread_priority_t priority)
{
    if (!fn)
        return NULL;
    ztk_thread *t = (ztk_thread *)calloc(1, sizeof(*t));
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

ztk_thread *ztk_thread_create(ztk_thread_fn fn, void *user)
{
    return ztk_thread_create_ex(fn, user, ZTK_THREAD_PRIO_NORMAL);
}

ztk_err_t ztk_thread_join(ztk_thread *t)
{
    if (!t)
        return ZTK_ERR_INVALID;
    void *ret = NULL;
    if (pthread_join(t->native, &ret) != 0)
        return ZTK_ERR_PLATFORM;
    return ZTK_OK;
}

void ztk_thread_destroy(ztk_thread *t)
{
    free(t);
}

unsigned int ztk_thread_hardware_concurrency(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;
    return (unsigned int)n;
}
