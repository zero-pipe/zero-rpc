#include "poller_internal.h"
#include "poller/poller.h"
#include "platform.h"
#include "ntk_errno.h"
#include <stdlib.h>

#if defined(_MSC_VER)
__declspec(thread) static ntk_poller *g_current_poller;
#else
static __thread ntk_poller *g_current_poller;
#endif

ntk_poller *ntk_poller_current(void)
{
    return g_current_poller;
}

void ntk_poller_task_init(ntk_poller *p)
{
    if (!p)
        return;
    p->task_mtx = ntk_mutex_create(NTK_MUTEX_NORMAL);
    p->task_head = NULL;
    p->task_tail = NULL;
    p->owner_thread = 0;
    ntk_poller_timer_init(p);
    ntk_poller_load_init(p);
}

void ntk_poller_task_fini(ntk_poller *p)
{
    if (!p)
        return;
    ntk_poller_load_fini(p);
    /* Drain queued async work (e.g. timer_cancel_async) while the timer heap
       and timer mutex are still valid, then release the heap. */
    ntk_poller_drain_tasks(p);
    ntk_poller_timer_fini(p);
    if (p->task_mtx) {
        ntk_mutex_destroy(p->task_mtx);
        p->task_mtx = NULL;
    }
}

void ntk_poller_drain_tasks(ntk_poller *p)
{
    if (!p || !p->task_mtx)
        return;

    ntk_poller_task *list = NULL;
    ntk_mutex_lock(p->task_mtx);
    list = p->task_head;
    p->task_head = NULL;
    p->task_tail = NULL;
    ntk_mutex_unlock(p->task_mtx);

    while (list) {
        ntk_poller_task *next = list->next;
        if (list->fn)
            list->fn(list->user);
        free(list);
        list = next;
    }
}

ntk_err_t ntk_poller_async_impl(ntk_poller *p, ntk_poller_task_fn fn, void *user, int may_sync, int push_front)
{
    if (!p || !fn)
        return NTK_ERR_INVALID;

    if (may_sync && p->owner_thread != 0 && p->owner_thread == ntk_thread_self_id()) {
        fn(user);
        return NTK_OK;
    }

    if (!p->task_mtx)
        return NTK_ERR_STATE;

    ntk_poller_task *task = (ntk_poller_task *)calloc(1, sizeof(*task));
    if (!task)
        return NTK_ERR_NOMEM;
    task->fn = fn;
    task->user = user;

    ntk_mutex_lock(p->task_mtx);
    if (push_front) {
        task->next = p->task_head;
        p->task_head = task;
        if (!p->task_tail)
            p->task_tail = task;
    } else {
        task->next = NULL;
        if (p->task_tail)
            p->task_tail->next = task;
        else
            p->task_head = task;
        p->task_tail = task;
    }
    ntk_mutex_unlock(p->task_mtx);

    return ntk_poller_wake(p);
}

int ntk_poller_is_current_thread(ntk_poller *p)
{
    if (!p || p->owner_thread == 0)
        return 0;
    return p->owner_thread == ntk_thread_self_id();
}

void ntk_poller_bind_thread(ntk_poller *p)
{
    if (p) {
        p->owner_thread = ntk_thread_self_id();
        g_current_poller = p;
    }
}

void ntk_poller_unbind_thread(ntk_poller *p)
{
    if (p) {
        p->owner_thread = 0;
        if (g_current_poller == p)
            g_current_poller = NULL;
    }
}

ntk_err_t ntk_poller_async(ntk_poller *p, ntk_poller_task_fn fn, void *user, int may_sync)
{
    return ntk_poller_async_impl(p, fn, user, may_sync, 0);
}

ntk_err_t ntk_poller_async_first(ntk_poller *p, ntk_poller_task_fn fn, void *user, int may_sync)
{
    return ntk_poller_async_impl(p, fn, user, may_sync, 1);
}

void ntk_poller_process_pending(ntk_poller *p)
{
    ntk_poller_drain_tasks(p);
}

void ntk_poller_run(ntk_poller *p, volatile int *stop_flag)
{
    if (!p)
        return;
    ntk_poller_bind_thread(p);
    while (!stop_flag || !*stop_flag) {
        int n = ntk_poller_poll(p, ntk_poller_resolve_timeout_ms(p, NTK_POLLER_PENDING_POLL_MS));
        if (n < 0)
            break;
    }
    ntk_poller_unbind_thread(p);
}
