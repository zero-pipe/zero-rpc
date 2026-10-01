#include "ztk/thread/thread.h"
#include <stdlib.h>
#include <windows.h>

struct ztk_thread {
    HANDLE h;
    ztk_thread_fn fn;
    void *user;
    ztk_thread_priority_t priority;
};

static DWORD WINAPI trampoline(LPVOID arg)
{
    ztk_thread *t = (ztk_thread *)arg;
    ztk_thread_set_current_priority(t->priority);
    t->fn(t->user);
    return 0;
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
    t->h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!t->h) {
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
    DWORD r = WaitForSingleObject(t->h, INFINITE);
    return r == WAIT_OBJECT_0 ? ZTK_OK : ZTK_ERR_PLATFORM;
}

void ztk_thread_destroy(ztk_thread *t)
{
    if (!t)
        return;
    if (t->h)
        CloseHandle(t->h);
    free(t);
}

unsigned int ztk_thread_hardware_concurrency(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors < 1)
        return 1;
    return (unsigned int)info.dwNumberOfProcessors;
}
