#include "ztk/thread/sync.h"
#include "ztk/ztk_errno.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct ztk_mutex {
    pthread_mutex_t native;
};

ztk_mutex *ztk_mutex_create(int flags)
{
    ztk_mutex *m = (ztk_mutex *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (flags & ZTK_MUTEX_RECURSIVE)
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    if (pthread_mutex_init(&m->native, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(m);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    return m;
}

void ztk_mutex_destroy(ztk_mutex *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->native);
    free(m);
}

ztk_err_t ztk_mutex_lock(ztk_mutex *m)
{
    if (!m)
        return ZTK_ERR_INVALID;
    return pthread_mutex_lock(&m->native) == 0 ? ZTK_OK : ZTK_ERR_PLATFORM;
}

ztk_err_t ztk_mutex_unlock(ztk_mutex *m)
{
    if (!m)
        return ZTK_ERR_INVALID;
    return pthread_mutex_unlock(&m->native) == 0 ? ZTK_OK : ZTK_ERR_PLATFORM;
}

ztk_err_t ztk_mutex_trylock(ztk_mutex *m)
{
    if (!m)
        return ZTK_ERR_INVALID;
    int r = pthread_mutex_trylock(&m->native);
    if (r == 0)
        return ZTK_OK;
    if (r == EBUSY)
        return ZTK_ERR_AGAIN;
    return ZTK_ERR_PLATFORM;
}
