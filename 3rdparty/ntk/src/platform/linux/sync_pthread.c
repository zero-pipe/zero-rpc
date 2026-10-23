#include "thread/sync.h"
#include "ntk_errno.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct ntk_mutex {
    pthread_mutex_t native;
};

ntk_mutex *ntk_mutex_create(int flags)
{
    ntk_mutex *m = (ntk_mutex *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (flags & NTK_MUTEX_RECURSIVE)
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    if (pthread_mutex_init(&m->native, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(m);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    return m;
}

void ntk_mutex_destroy(ntk_mutex *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->native);
    free(m);
}

ntk_err_t ntk_mutex_lock(ntk_mutex *m)
{
    if (!m)
        return NTK_ERR_INVALID;
    return pthread_mutex_lock(&m->native) == 0 ? NTK_OK : NTK_ERR_PLATFORM;
}

ntk_err_t ntk_mutex_unlock(ntk_mutex *m)
{
    if (!m)
        return NTK_ERR_INVALID;
    return pthread_mutex_unlock(&m->native) == 0 ? NTK_OK : NTK_ERR_PLATFORM;
}

ntk_err_t ntk_mutex_trylock(ntk_mutex *m)
{
    if (!m)
        return NTK_ERR_INVALID;
    int r = pthread_mutex_trylock(&m->native);
    if (r == 0)
        return NTK_OK;
    if (r == EBUSY)
        return NTK_ERR_AGAIN;
    return NTK_ERR_PLATFORM;
}
