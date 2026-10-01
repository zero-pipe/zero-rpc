#include "../poller_internal.h"
#include "../../net/internal/sock_platform.h"
#include "ztk/poller/poller.h"
#include "ztk/ztk_errno.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <string.h>

#define ZTK_EPOLL_MAX_EVENTS 64

struct ztk_fd_entry {
    int fd;
    unsigned events;
    ztk_poller_cb cb;
    void *user;
    struct ztk_fd_entry *next;
};

static HANDLE ep_handle(ztk_poller *p)
{
    return (HANDLE)(intptr_t)p->epfd;
}

static ztk_fd_entry *find_entry(ztk_poller *p, int fd)
{
    for (ztk_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd == fd)
            return e;
    }
    return NULL;
}

static int set_nonblock(SOCKET s, int on)
{
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode);
}

/** TCP 回环 socket 对作唤醒通道（wepoll 仅支持 socket） */
static int make_wake_pair(int fds[2])
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(listener);
        return -1;
    }
    if (listen(listener, 1) != 0) {
        closesocket(listener);
        return -1;
    }

    int len = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
        closesocket(listener);
        return -1;
    }

    SOCKET writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (writer == INVALID_SOCKET) {
        closesocket(listener);
        return -1;
    }
    if (connect(writer, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(writer);
        closesocket(listener);
        return -1;
    }

    SOCKET reader = accept(listener, NULL, NULL);
    closesocket(listener);
    if (reader == INVALID_SOCKET) {
        closesocket(writer);
        return -1;
    }

    set_nonblock(reader, 1);
    set_nonblock(writer, 1);
    fds[0] = (int)reader;
    fds[1] = (int)writer;
    return 0;
}

static ztk_err_t epoll_ctl_wrap(HANDLE ep, int op, SOCKET sock, uint32_t ev)
{
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events = ev;
    ee.data.fd = (int)sock;
    if (epoll_ctl(ep, op, sock, &ee) != 0)
        return ZTK_ERR_PLATFORM;
    return ZTK_OK;
}

static uint32_t to_epoll(unsigned events)
{
    uint32_t ev = 0;
    if (events & ZTK_POLL_IN)
        ev |= EPOLLIN;
    if (events & ZTK_POLL_OUT)
        ev |= EPOLLOUT;
    if (events & ZTK_POLL_ERR)
        ev |= EPOLLERR;
    if (events & ZTK_POLL_HUP)
        ev |= EPOLLHUP;
    return ev;
}

static void drain_wake_pipe(ztk_poller *p)
{
    char buf[256];
    while (recv(p->wake_r, buf, (int)sizeof(buf), 0) > 0) {}
}

ztk_poller *ztk_poller_create(void)
{
    if (ztk_sockplat_init() != ZTK_OK)
        return NULL;

    ztk_poller *p = (ztk_poller *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    p->epfd = 0;
    p->wake_r = -1;
    p->wake_w = -1;
    ztk_poller_task_init(p);

    HANDLE ep = epoll_create1(0);
    if (!ep)
        goto fail;
    p->epfd = (intptr_t)ep;

    int pipes[2];
    if (make_wake_pair(pipes) != 0)
        goto fail;
    p->wake_r = pipes[0];
    p->wake_w = pipes[1];

    ztk_fd_entry *we = (ztk_fd_entry *)calloc(1, sizeof(*we));
    if (!we)
        goto fail;
    we->fd = p->wake_r;
    we->events = ZTK_POLL_IN;
    we->next = p->entries;
    p->entries = we;

    if (epoll_ctl_wrap(ep, EPOLL_CTL_ADD, (SOCKET)p->wake_r, EPOLLIN) != ZTK_OK)
        goto fail;
    return p;

fail:
    ztk_poller_destroy(p);
    return NULL;
}

void ztk_poller_destroy(ztk_poller *p)
{
    if (!p)
        return;

    ztk_poller_task_fini(p);

    while (p->entries) {
        ztk_fd_entry *n = p->entries->next;
        free(p->entries);
        p->entries = n;
    }
    if (p->wake_w >= 0)
        closesocket((SOCKET)p->wake_w);
    if (p->wake_r >= 0)
        closesocket((SOCKET)p->wake_r);
    if (p->epfd)
        epoll_close(ep_handle(p));
    free(p);
}

ztk_err_t ztk_poller_add(ztk_poller *p, int fd, unsigned events, ztk_poller_cb cb, void *user)
{
    if (!p || fd < 0)
        return ZTK_ERR_INVALID;

    HANDLE ep = ep_handle(p);
    ztk_fd_entry *e = find_entry(p, fd);
    if (e) {
        e->events = events;
        e->cb = cb;
        e->user = user;
        return epoll_ctl_wrap(ep, EPOLL_CTL_MOD, (SOCKET)fd, to_epoll(events));
    }

    e = (ztk_fd_entry *)calloc(1, sizeof(*e));
    if (!e)
        return ZTK_ERR_NOMEM;
    e->fd = fd;
    e->events = events;
    e->cb = cb;
    e->user = user;
    e->next = p->entries;
    p->entries = e;

    if (epoll_ctl_wrap(ep, EPOLL_CTL_ADD, (SOCKET)fd, to_epoll(events)) != ZTK_OK) {
        p->entries = e->next;
        free(e);
        return ZTK_ERR_PLATFORM;
    }
    return ZTK_OK;
}

ztk_err_t ztk_poller_mod(ztk_poller *p, int fd, unsigned events)
{
    if (!p)
        return ZTK_ERR_INVALID;
    ztk_fd_entry *e = find_entry(p, fd);
    if (!e)
        return ZTK_ERR_INVALID;
    e->events = events;
    return epoll_ctl_wrap(ep_handle(p), EPOLL_CTL_MOD, (SOCKET)fd, to_epoll(events));
}

ztk_err_t ztk_poller_del(ztk_poller *p, int fd)
{
    if (!p)
        return ZTK_ERR_INVALID;
    if (fd == p->wake_r)
        return ZTK_ERR_INVALID;

    ztk_fd_entry **prev = &p->entries;
    for (ztk_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd == fd) {
            *prev = e->next;
            epoll_ctl(ep_handle(p), EPOLL_CTL_DEL, (SOCKET)fd, NULL);
            free(e);
            return ZTK_OK;
        }
        prev = &e->next;
    }
    return ZTK_ERR_INVALID;
}

int ztk_poller_poll(ztk_poller *p, int timeout_ms)
{
    if (!p || !p->epfd)
        return -1;

    struct epoll_event evs[ZTK_EPOLL_MAX_EVENTS];
    int wait_ms = ztk_poller_resolve_timeout_ms(p, timeout_ms);
    ztk_poller_load_on_sleep(p);
    int n = epoll_wait(ep_handle(p), evs, ZTK_EPOLL_MAX_EVENTS, wait_ms);
    ztk_poller_load_on_wake(p);
    if (n < 0)
        return -1;

    if (n == 0 && p->task_mtx) {
        int pending = 0;
        ztk_mutex_lock(p->task_mtx);
        pending = p->task_head != NULL;
        ztk_mutex_unlock(p->task_mtx);
        if (pending)
            ztk_poller_drain_tasks(p);
    }

    for (int i = 0; i < n; ++i) {
        int fd = evs[i].data.fd;
        if (fd == p->wake_r) {
            drain_wake_pipe(p);
            ztk_poller_drain_tasks(p);
        }

        ztk_fd_entry *e = find_entry(p, fd);
        if (!e || !e->cb)
            continue;

        unsigned out = 0;
        if (evs[i].events & EPOLLIN)
            out |= ZTK_POLL_IN;
        if (evs[i].events & EPOLLOUT)
            out |= ZTK_POLL_OUT;
        if (evs[i].events & EPOLLERR)
            out |= ZTK_POLL_ERR;
        if (evs[i].events & EPOLLHUP)
            out |= ZTK_POLL_HUP;
        e->cb(fd, out, e->user);
    }

    if (p->task_mtx) {
        int pending = 0;
        ztk_mutex_lock(p->task_mtx);
        pending = p->task_head != NULL;
        ztk_mutex_unlock(p->task_mtx);
        if (pending)
            ztk_poller_drain_tasks(p);
    }
    return n;
}

int ztk_poller_wake_read_fd(ztk_poller *p)
{
    return p ? p->wake_r : -1;
}

int ztk_poller_wake_write_fd(ztk_poller *p)
{
    return p ? p->wake_w : -1;
}

ztk_err_t ztk_poller_wake(ztk_poller *p)
{
    if (!p || p->wake_w < 0)
        return ZTK_ERR_INVALID;
    char c = 1;
    if (send(p->wake_w, &c, 1, 0) < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return ZTK_OK;
        return ZTK_ERR_PLATFORM;
    }
    return ZTK_OK;
}
