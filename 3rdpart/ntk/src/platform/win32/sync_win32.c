#include "thread/sync.h"
#include <stdlib.h>
#include <windows.h>

struct ntk_mutex {
    CRITICAL_SECTION cs;
    int recursive;
};

ntk_mutex *ntk_mutex_create(int flags)
{
    ntk_mutex *m = (ntk_mutex *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->recursive = (flags & NTK_MUTEX_RECURSIVE) ? 1 : 0;
    InitializeCriticalSection(&m->cs);
    return m;
}

void ntk_mutex_destroy(ntk_mutex *m)
{
    if (!m)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

ntk_err_t ntk_mutex_lock(ntk_mutex *m)
{
    if (!m)
        return NTK_ERR_INVALID;
    EnterCriticalSection(&m->cs);
    return NTK_OK;
}

ntk_err_t ntk_mutex_unlock(ntk_mutex *m)
{
    if (!m)
        return NTK_ERR_INVALID;
    LeaveCriticalSection(&m->cs);
    return NTK_OK;
}

ntk_err_t ntk_mutex_trylock(ntk_mutex *m)
{
    if (!m)
        return NTK_ERR_INVALID;
    return TryEnterCriticalSection(&m->cs) ? NTK_OK : NTK_ERR_AGAIN;
}
