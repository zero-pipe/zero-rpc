#include "thread/sem.h"
#include <stdlib.h>
#include <windows.h>

struct ntk_sem {
    HANDLE h;
};

ntk_sem *ntk_sem_create(unsigned int initial)
{
    ntk_sem *s = (ntk_sem *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->h = CreateSemaphoreA(NULL, (LONG)initial, 0x7fffffff, NULL);
    if (!s->h) {
        free(s);
        return NULL;
    }
    return s;
}

void ntk_sem_destroy(ntk_sem *s)
{
    if (!s)
        return;
    if (s->h)
        CloseHandle(s->h);
    free(s);
}

ntk_err_t ntk_sem_wait(ntk_sem *s)
{
    if (!s)
        return NTK_ERR_INVALID;
    return WaitForSingleObject(s->h, INFINITE) == WAIT_OBJECT_0 ? NTK_OK : NTK_ERR_PLATFORM;
}

ntk_err_t ntk_sem_timedwait(ntk_sem *s, unsigned int timeout_ms)
{
    if (!s)
        return NTK_ERR_INVALID;
    DWORD r = WaitForSingleObject(s->h, timeout_ms);
    if (r == WAIT_OBJECT_0)
        return NTK_OK;
    if (r == WAIT_TIMEOUT)
        return NTK_ERR_TIMEOUT;
    return NTK_ERR_PLATFORM;
}

ntk_err_t ntk_sem_post(ntk_sem *s, unsigned int count)
{
    if (!s || !count)
        return NTK_ERR_INVALID;
    return ReleaseSemaphore(s->h, (LONG)count, NULL) ? NTK_OK : NTK_ERR_PLATFORM;
}
