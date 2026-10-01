#include "ztk/thread/sync.h"
#include <stdlib.h>
#include <windows.h>

struct ztk_mutex {
    CRITICAL_SECTION cs;
    int recursive;
};

ztk_mutex *ztk_mutex_create(int flags)
{
    ztk_mutex *m = (ztk_mutex *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->recursive = (flags & ZTK_MUTEX_RECURSIVE) ? 1 : 0;
    InitializeCriticalSection(&m->cs);
    return m;
}

void ztk_mutex_destroy(ztk_mutex *m)
{
    if (!m)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

ztk_err_t ztk_mutex_lock(ztk_mutex *m)
{
    if (!m)
        return ZTK_ERR_INVALID;
    EnterCriticalSection(&m->cs);
    return ZTK_OK;
}

ztk_err_t ztk_mutex_unlock(ztk_mutex *m)
{
    if (!m)
        return ZTK_ERR_INVALID;
    LeaveCriticalSection(&m->cs);
    return ZTK_OK;
}

ztk_err_t ztk_mutex_trylock(ztk_mutex *m)
{
    if (!m)
        return ZTK_ERR_INVALID;
    return TryEnterCriticalSection(&m->cs) ? ZTK_OK : ZTK_ERR_AGAIN;
}
