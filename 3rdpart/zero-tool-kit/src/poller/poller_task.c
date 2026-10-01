#include "poller_internal.h"
#include "ztk/poller/poller.h"
#include "ztk/platform.h"
#include "ztk/ztk_errno.h"
#include <stdlib.h>

void ztk_poller_task_init(ztk_poller *p)
{
    if (!p)
        return;
    p->task_mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    p->task_head = NULL;
    p->task_tail = NULL;
    p->owner_thread = 0;
    ztk_poller_timer_init(p);
    ztk_poller_load_init(p);
}

void ztk_poller_task_fini(ztk_poller *p)
{
    if (!p)
        return;
    ztk_poller_load_fini(p);
    /* Drain queued async work (e.g. timer_cancel_async) while the timer heap
       and timer mutex are still valid, then release the heap. */
    ztk_poller_drain_tasks(p);
    ztk_poller_timer_fini(p);
    if (p->task_mtx) {
        ztk_mutex_destroy(p->task_mtx);
        p->task_mtx = NULL;
    }
}

void ztk_poller_drain_tasks(ztk_poller *p)
{
    if (!p || !p->task_mtx)
        return;

    ztk_poller_task *list = NULL;
    ztk_mutex_lock(p->task_mtx);
    list = p->task_head;
    p->task_head = NULL;
    p->task_tail = NULL;
    ztk_mutex_unlock(p->task_mtx);

    while (list) {
        ztk_poller_task *next = list->next;
        if (list->fn)
            list->fn(list->user);
        free(list);
        list = next;
    }
}

ztk_err_t ztk_poller_async_impl(ztk_poller *p, ztk_poller_task_fn fn, void *user, int may_sync, int push_front)
{
    if (!p || !fn)
        return ZTK_ERR_INVALID;

    if (may_sync && p->owner_thread != 0 && p->owner_thread == ztk_thread_self_id()) {
        fn(user);
        return ZTK_OK;
    }

    if (!p->task_mtx)
        return ZTK_ERR_STATE;

    ztk_poller_task *task = (ztk_poller_task *)calloc(1, sizeof(*task));
    if (!task)
        return ZTK_ERR_NOMEM;
    task->fn = fn;
    task->user = user;

    ztk_mutex_lock(p->task_mtx);
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
    ztk_mutex_unlock(p->task_mtx);

    return ztk_poller_wake(p);
}

int ztk_poller_is_current_thread(ztk_poller *p)
{
    if (!p || p->owner_thread == 0)
        return 0;
    return p->owner_thread == ztk_thread_self_id();
}

void ztk_poller_bind_thread(ztk_poller *p)
{
    if (p)
        p->owner_thread = ztk_thread_self_id();
}

void ztk_poller_unbind_thread(ztk_poller *p)
{
    if (p)
        p->owner_thread = 0;
}

ztk_err_t ztk_poller_async(ztk_poller *p, ztk_poller_task_fn fn, void *user, int may_sync)
{
    return ztk_poller_async_impl(p, fn, user, may_sync, 0);
}

ztk_err_t ztk_poller_async_first(ztk_poller *p, ztk_poller_task_fn fn, void *user, int may_sync)
{
    return ztk_poller_async_impl(p, fn, user, may_sync, 1);
}

void ztk_poller_process_pending(ztk_poller *p)
{
    ztk_poller_drain_tasks(p);
}

void ztk_poller_run(ztk_poller *p, volatile int *stop_flag)
{
    if (!p)
        return;
    ztk_poller_bind_thread(p);
    while (!stop_flag || !*stop_flag) {
        int n = ztk_poller_poll(p, ztk_poller_resolve_timeout_ms(p, ZTK_POLLER_PENDING_POLL_MS));
        if (n < 0)
            break;
    }
    ztk_poller_unbind_thread(p);
}
