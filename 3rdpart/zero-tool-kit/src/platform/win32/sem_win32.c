#include "ztk/thread/sem.h"
#include <stdlib.h>
#include <windows.h>

struct ztk_sem {
    HANDLE h;
};

ztk_sem *ztk_sem_create(unsigned int initial)
{
    ztk_sem *s = (ztk_sem *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->h = CreateSemaphoreA(NULL, (LONG)initial, 0x7fffffff, NULL);
    if (!s->h) {
        free(s);
        return NULL;
    }
    return s;
}

void ztk_sem_destroy(ztk_sem *s)
{
    if (!s)
        return;
    if (s->h)
        CloseHandle(s->h);
    free(s);
}

ztk_err_t ztk_sem_wait(ztk_sem *s)
{
    if (!s)
        return ZTK_ERR_INVALID;
    return WaitForSingleObject(s->h, INFINITE) == WAIT_OBJECT_0 ? ZTK_OK : ZTK_ERR_PLATFORM;
}

ztk_err_t ztk_sem_timedwait(ztk_sem *s, unsigned int timeout_ms)
{
    if (!s)
        return ZTK_ERR_INVALID;
    DWORD r = WaitForSingleObject(s->h, timeout_ms);
    if (r == WAIT_OBJECT_0)
        return ZTK_OK;
    if (r == WAIT_TIMEOUT)
        return ZTK_ERR_TIMEOUT;
    return ZTK_ERR_PLATFORM;
}

ztk_err_t ztk_sem_post(ztk_sem *s, unsigned int count)
{
    if (!s || !count)
        return ZTK_ERR_INVALID;
    return ReleaseSemaphore(s->h, (LONG)count, NULL) ? ZTK_OK : ZTK_ERR_PLATFORM;
}
