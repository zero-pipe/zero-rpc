#include "thread/thread.h"
#include <stdlib.h>
#include <windows.h>

struct ntk_thread {
    HANDLE h;
    ntk_thread_fn fn;
    void *user;
    ntk_thread_priority_t priority;
};

static DWORD WINAPI trampoline(LPVOID arg)
{
    ntk_thread *t = (ntk_thread *)arg;
    ntk_thread_set_current_priority(t->priority);
    t->fn(t->user);
    return 0;
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
    t->h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!t->h) {
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
    DWORD r = WaitForSingleObject(t->h, INFINITE);
    return r == WAIT_OBJECT_0 ? NTK_OK : NTK_ERR_PLATFORM;
}

void ntk_thread_destroy(ntk_thread *t)
{
    if (!t)
        return;
    if (t->h)
        CloseHandle(t->h);
    free(t);
}

unsigned int ntk_thread_hardware_concurrency(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors < 1)
        return 1;
    return (unsigned int)info.dwNumberOfProcessors;
}
