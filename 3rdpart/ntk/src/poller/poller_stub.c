#include "poller_internal.h"
#include "ztk/poller/poller.h"
#include <stdlib.h>

/* resolve_timeout declared in poller_internal.h */

ztk_poller *ztk_poller_create(void)
{
    ztk_poller *p = (ztk_poller *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->epfd = -1;
    p->wake_r = -1;
    p->wake_w = -1;
    ztk_poller_task_init(p);
    p->epfd = -1;
    return p;
}

void ztk_poller_destroy(ztk_poller *p)
{
    if (!p)
        return;
    ztk_poller_task_fini(p);
    free(p);
}

ztk_err_t ztk_poller_add(ztk_poller *p, int fd, unsigned events, ztk_poller_cb cb, void *user)
{
    (void)p;
    (void)fd;
    (void)events;
    (void)cb;
    (void)user;
    return ZTK_ERR_NOT_IMPL;
}

ztk_err_t ztk_poller_mod(ztk_poller *p, int fd, unsigned events)
{
    (void)p;
    (void)fd;
    (void)events;
    return ZTK_ERR_NOT_IMPL;
}

ztk_err_t ztk_poller_del(ztk_poller *p, int fd)
{
    (void)p;
    (void)fd;
    return ZTK_ERR_NOT_IMPL;
}

int ztk_poller_poll(ztk_poller *p, int timeout_ms)
{
    (void)timeout_ms;
    if (!p)
        return -1;
    (void)ztk_poller_resolve_timeout_ms(p, timeout_ms);
    ztk_poller_drain_tasks(p);
    return 0;
}

int ztk_poller_wake_read_fd(ztk_poller *p)
{
    (void)p;
    return -1;
}

int ztk_poller_wake_write_fd(ztk_poller *p)
{
    (void)p;
    return -1;
}

ztk_err_t ztk_poller_wake(ztk_poller *p)
{
    if (!p)
        return ZTK_ERR_INVALID;
    /* 无 pipe 时仅排队，由 poll/process_pending 排空 */
    return ZTK_OK;
}
