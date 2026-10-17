#include "poller_internal.h"
#include "poller/poller.h"
#include <stdlib.h>

/* resolve_timeout declared in poller_internal.h */

ntk_poller *ntk_poller_create(void)
{
    ntk_poller *p = (ntk_poller *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->epfd = -1;
    p->wake_r = -1;
    p->wake_w = -1;
    ntk_poller_task_init(p);
    p->epfd = -1;
    return p;
}

void ntk_poller_destroy(ntk_poller *p)
{
    if (!p)
        return;
    ntk_poller_task_fini(p);
    free(p);
}

ntk_err_t ntk_poller_add(ntk_poller *p, int fd, unsigned events, ntk_poller_cb cb, void *user)
{
    (void)p;
    (void)fd;
    (void)events;
    (void)cb;
    (void)user;
    return NTK_ERR_NOT_IMPL;
}

ntk_err_t ntk_poller_mod(ntk_poller *p, int fd, unsigned events)
{
    (void)p;
    (void)fd;
    (void)events;
    return NTK_ERR_NOT_IMPL;
}

ntk_err_t ntk_poller_del(ntk_poller *p, int fd)
{
    (void)p;
    (void)fd;
    return NTK_ERR_NOT_IMPL;
}

int ntk_poller_poll(ntk_poller *p, int timeout_ms)
{
    (void)timeout_ms;
    if (!p)
        return -1;
    (void)ntk_poller_resolve_timeout_ms(p, timeout_ms);
    ntk_poller_drain_tasks(p);
    return 0;
}

int ntk_poller_wake_read_fd(ntk_poller *p)
{
    (void)p;
    return -1;
}

int ntk_poller_wake_write_fd(ntk_poller *p)
{
    (void)p;
    return -1;
}

ntk_err_t ntk_poller_wake(ntk_poller *p)
{
    if (!p)
        return NTK_ERR_INVALID;
    /* 无 pipe 时仅排队，由 poll/process_pending 排空 */
    return NTK_OK;
}
