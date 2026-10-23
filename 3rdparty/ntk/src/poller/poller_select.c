#include "poller_internal.h"
#include "poller/poller.h"
#include "ntk_errno.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#  include <winsock2.h>
#  define NTK_SELECT_CLOSE(fd)           closesocket((SOCKET)(fd))
#  define NTK_SELECT_READ(fd, buf, len)  recv((SOCKET)(fd), (buf), (int)(len), 0)
#  define NTK_SELECT_WRITE(fd, buf, len) send((SOCKET)(fd), (buf), (int)(len), 0)
#  define NTK_SELECT_WOULDBLOCK(e)       ((e) == WSAEWOULDBLOCK)
#  define NTK_SELECT_INTR(e)             ((e) == WSAEINTR)
static int select_last_err(void) { return WSAGetLastError(); }
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/select.h>
#  define NTK_SELECT_CLOSE(fd)           close(fd)
#  define NTK_SELECT_READ(fd, buf, len)  read((fd), (buf), (len))
#  define NTK_SELECT_WRITE(fd, buf, len) write((fd), (buf), (len))
#  define NTK_SELECT_WOULDBLOCK(e)       ((e) == EAGAIN || (e) == EWOULDBLOCK)
#  define NTK_SELECT_INTR(e)             ((e) == EINTR)
static int select_last_err(void) { return errno; }
#endif

struct ntk_fd_entry {
    int fd;
    unsigned events;
    ntk_poller_cb cb;
    void *user;
    struct ntk_fd_entry *next;
};

static ntk_fd_entry *find_entry(ntk_poller *p, int fd)
{
    for (ntk_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd == fd)
            return e;
    }
    return NULL;
}

/* --- wake pair ----------------------------------------------------------- */

#ifdef _WIN32
static int make_wake_pair(int fds[2])
{
    /* winsock: loopback TCP pair（与 poller_wepoll.c 相同做法） */
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0) {
        closesocket(listener);
        return -1;
    }
    int len = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
        closesocket(listener);
        return -1;
    }

    SOCKET writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (writer == INVALID_SOCKET) { closesocket(listener); return -1; }
    if (connect(writer, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(writer); closesocket(listener); return -1;
    }
    SOCKET reader = accept(listener, NULL, NULL);
    closesocket(listener);
    if (reader == INVALID_SOCKET) { closesocket(writer); return -1; }

    u_long nb = 1;
    ioctlsocket(reader, FIONBIO, &nb);
    ioctlsocket(writer, FIONBIO, &nb);
    fds[0] = (int)reader;
    fds[1] = (int)writer;
    return 0;
}
#else
static int make_wake_pair(int fds[2])
{
#ifdef NTK_HAVE_PIPE2
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0)
        return 0;
#endif
    if (pipe(fds) != 0)
        return -1;
    for (int i = 0; i < 2; ++i) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
    }
    return 0;
}
#endif

static void drain_wake(ntk_poller *p)
{
    char buf[256];
    while (NTK_SELECT_READ(p->wake_r, buf, sizeof(buf)) > 0) {}
}

/* --- public API ---------------------------------------------------------- */

ntk_poller *ntk_poller_create(void)
{
    ntk_poller *p = (ntk_poller *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    p->epfd   = -1;
    p->wake_r = -1;
    p->wake_w = -1;
    ntk_poller_task_init(p);

    int pipes[2];
    if (make_wake_pair(pipes) != 0)
        goto fail;
    p->wake_r = pipes[0];
    p->wake_w = pipes[1];

    ntk_fd_entry *we = (ntk_fd_entry *)calloc(1, sizeof(*we));
    if (!we)
        goto fail;
    we->fd     = p->wake_r;
    we->events = NTK_POLL_IN;
    we->next   = p->entries;
    p->entries = we;

    return p;

fail:
    ntk_poller_destroy(p);
    return NULL;
}

void ntk_poller_destroy(ntk_poller *p)
{
    if (!p)
        return;
    ntk_poller_task_fini(p);
    while (p->entries) {
        ntk_fd_entry *n = p->entries->next;
        free(p->entries);
        p->entries = n;
    }
    if (p->wake_w >= 0) NTK_SELECT_CLOSE(p->wake_w);
    if (p->wake_r >= 0) NTK_SELECT_CLOSE(p->wake_r);
    free(p);
}

ntk_err_t ntk_poller_add(ntk_poller *p, int fd, unsigned events, ntk_poller_cb cb, void *user)
{
    if (!p || fd < 0)
        return NTK_ERR_INVALID;

    ntk_fd_entry *e = find_entry(p, fd);
    if (e) {
        e->events = events;
        e->cb     = cb;
        e->user   = user;
        return NTK_OK;
    }

    e = (ntk_fd_entry *)calloc(1, sizeof(*e));
    if (!e)
        return NTK_ERR_NOMEM;
    e->fd     = fd;
    e->events = events;
    e->cb     = cb;
    e->user   = user;
    e->next   = p->entries;
    p->entries = e;
    return NTK_OK;
}

ntk_err_t ntk_poller_mod(ntk_poller *p, int fd, unsigned events)
{
    if (!p)
        return NTK_ERR_INVALID;
    ntk_fd_entry *e = find_entry(p, fd);
    if (!e)
        return NTK_ERR_INVALID;
    e->events = events;
    return NTK_OK;
}

ntk_err_t ntk_poller_del(ntk_poller *p, int fd)
{
    if (!p)
        return NTK_ERR_INVALID;
    if (fd == p->wake_r)
        return NTK_ERR_INVALID;

    ntk_fd_entry **prev = &p->entries;
    for (ntk_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd == fd) {
            *prev = e->next;
            free(e);
            return NTK_OK;
        }
        prev = &e->next;
    }
    return NTK_ERR_INVALID;
}

int ntk_poller_poll(ntk_poller *p, int timeout_ms)
{
    if (!p)
        return -1;

    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    int maxfd = -1;
    for (ntk_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd < 0)
            continue;
#ifdef _WIN32
        (void)maxfd;
#else
        if (e->fd >= FD_SETSIZE)
            continue;
        if (e->fd > maxfd)
            maxfd = e->fd;
#endif
        if (e->events & (NTK_POLL_IN | NTK_POLL_HUP))
            FD_SET((unsigned)e->fd, &rfds);
        if (e->events & NTK_POLL_OUT)
            FD_SET((unsigned)e->fd, &wfds);
        if (e->events & NTK_POLL_ERR)
            FD_SET((unsigned)e->fd, &efds);
    }

    int wait_ms = ntk_poller_resolve_timeout_ms(p, timeout_ms);
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (wait_ms >= 0) {
        tv.tv_sec  = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;
        tvp = &tv;
    }

    ntk_poller_load_on_sleep(p);
#ifdef _WIN32
    int n = select(0, &rfds, &wfds, &efds, tvp);
#else
    int n = select(maxfd + 1, &rfds, &wfds, &efds, tvp);
#endif
    ntk_poller_load_on_wake(p);

    if (n < 0) {
        int e = select_last_err();
        if (NTK_SELECT_WOULDBLOCK(e) || NTK_SELECT_INTR(e))
            return 0;
        return -1;
    }
    if (n == 0) {
        if (p->task_mtx) {
            int pending = 0;
            ntk_mutex_lock(p->task_mtx);
            pending = p->task_head != NULL;
            ntk_mutex_unlock(p->task_mtx);
            if (pending)
                ntk_poller_drain_tasks(p);
        }
        return 0;
    }

    /* 先收集就绪事件，再触发回调，防止回调里 add/del 破坏链表遍历 */
    typedef struct { int fd; unsigned out; ntk_poller_cb cb; void *user; } ready_ev;
    ready_ev ready[64];
    int fired = 0;

    for (ntk_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd < 0)
            continue;
#ifndef _WIN32
        if (e->fd >= FD_SETSIZE)
            continue;
#endif
        if (e->fd == p->wake_r && FD_ISSET((unsigned)e->fd, &rfds)) {
            drain_wake(p);
            ntk_poller_drain_tasks(p);
            continue;
        }

        unsigned out = 0;
        if (FD_ISSET((unsigned)e->fd, &rfds)) out |= NTK_POLL_IN;
        if (FD_ISSET((unsigned)e->fd, &wfds)) out |= NTK_POLL_OUT;
        if (FD_ISSET((unsigned)e->fd, &efds)) out |= NTK_POLL_ERR;
        if (!out || !e->cb)
            continue;

        if (fired < (int)(sizeof(ready) / sizeof(ready[0]))) {
            ready[fired].fd   = e->fd;
            ready[fired].out  = out;
            ready[fired].cb   = e->cb;
            ready[fired].user = e->user;
        }
        ++fired;
    }

    int cap = fired < (int)(sizeof(ready) / sizeof(ready[0]))
              ? fired : (int)(sizeof(ready) / sizeof(ready[0]));
    for (int i = 0; i < cap; ++i)
        ready[i].cb(ready[i].fd, ready[i].out, ready[i].user);

    return fired;
}

int ntk_poller_wake_read_fd(ntk_poller *p)
{
    return p ? p->wake_r : -1;
}

int ntk_poller_wake_write_fd(ntk_poller *p)
{
    return p ? p->wake_w : -1;
}

ntk_err_t ntk_poller_wake(ntk_poller *p)
{
    if (!p || p->wake_w < 0)
        return NTK_ERR_INVALID;
    char c = 1;
    if (NTK_SELECT_WRITE(p->wake_w, &c, 1) < 0) {
        if (NTK_SELECT_WOULDBLOCK(select_last_err()))
            return NTK_OK;
        return NTK_ERR_PLATFORM;
    }
    return NTK_OK;
}
