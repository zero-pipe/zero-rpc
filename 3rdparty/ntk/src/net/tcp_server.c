#include "net/tcp_server.h"
#include "net/net_limits.h"
#include "util/buf.h"
#include "util/timer.h"
#include "poller/poller.h"
#include "poller/poller_pool.h"
#include "net/socket.h"
#include "thread/sync.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

#define NTK_TCP_RECV_CHUNK 4096
#define NTK_TCP_DEFAULT_MANAGER_SEC 2.0f

#ifdef NTK_TEST_HOOKS
static volatile size_t g_ntk_test_out_mem_append;
#endif

typedef struct ntk_listen_binding {
    ntk_tcp_server *server;
    ntk_poller *poller;
    ntk_poller_timer *manager_timer;
} ntk_listen_binding;

enum {
    NTK_TCP_OUT_MEM = 0,
    NTK_TCP_OUT_BUF = 1
};

typedef struct ntk_tcp_out_chunk {
    struct ntk_tcp_out_chunk *next;
    int kind;
    size_t off;
    size_t len;
    union {
        uint8_t *mem;
        ntk_buf *buf;
    } u;
} ntk_tcp_out_chunk;

struct ntk_tcp_session {
    ntk_tcp_server *server;
    ntk_socket *sock;
    ntk_poller *poller;
    void *user;
    int closing;
    ntk_tcp_out_chunk *out_head;
    ntk_tcp_out_chunk *out_tail;
    size_t out_pending;
    int out_highwater_hit;
};

struct ntk_tcp_server {
    ntk_socket *listen_sock;
    ntk_poller **pollers;
    unsigned poller_count;

    ntk_tcp_session_ops_t ops;
    void *session_user;
    void *(*session_create_user)(ntk_tcp_server *, ntk_tcp_session *);
    ntk_poller *(*pick_poller)(ntk_tcp_server *, ntk_poller *, void *);
    void *pick_poller_user;
    ntk_poller_pool *poller_pool;
    int pool_pick_poller;

    float manager_interval_sec;

    ntk_listen_binding *listen_bindings;
    ntk_tcp_session **sessions;
    size_t session_count;
    size_t session_cap;

    ntk_mutex *mutex;
    char host[NTK_NET_ADDR_STR_LEN];
    uint16_t port;
    int backlog;
    int started;

    size_t out_highwater_bytes;
    ntk_tcp_session_out_highwater_cb on_out_highwater;
};

static void session_detach(ntk_tcp_session *session);
static void session_remove(ntk_tcp_server *srv, ntk_tcp_session *session);
static void session_out_flush(ntk_tcp_session *session);

/*
 * 出站队列非空时打开 EPOLLOUT，队列排空后关闭。
 * 否则一旦 send 返回 EAGAIN，队列无人驱动（回退仅靠 manager 定时器，
 * 而 rpc 默认不启用 manager），大包会话会长时间停滞。
 */
static void session_update_out_events(ntk_tcp_session *session)
{
    if (!session || !session->sock || session->closing)
        return;
    if (session->out_pending > 0)
        ntk_socket_mod_events(session->sock, NTK_POLL_IN | NTK_POLL_ERR | NTK_POLL_HUP | NTK_POLL_OUT);
    else
        ntk_socket_mod_events(session->sock, NTK_POLL_IN | NTK_POLL_ERR | NTK_POLL_HUP);
}

static void session_on_writable(ntk_socket *sock, void *user)
{
    ntk_tcp_session *session = (ntk_tcp_session *)user;
    (void)sock;
    if (!session || session->closing)
        return;
    session_out_flush(session);
}

static void session_on_error(ntk_socket *sock, void *user)
{
    ntk_tcp_session *session = (ntk_tcp_session *)user;
    (void)sock;
    if (!session || session->closing)
        return;
    session_remove(session->server, session);
}

static void session_on_readable(ntk_socket *sock, void *user)
{
    ntk_tcp_session *session = (ntk_tcp_session *)user;
    if (!session || session->closing)
        return;

    for (;;) {
        char buf[NTK_TCP_RECV_CHUNK];
        ntk_ssize_t n = ntk_socket_recv(sock, buf, sizeof(buf));
        if (n > 0) {
            if (session->server->ops.on_recv)
                session->server->ops.on_recv(session, buf, (size_t)n, session->user);
            continue;
        }
        if (n == NTK_ERR_AGAIN)
            break;
        if (n == 0 || n < 0)
            session_remove(session->server, session);
        break;
    }
}

static ntk_err_t session_attach(ntk_tcp_session *session, ntk_poller *poller)
{
    ntk_socket_callbacks_t cb = { session_on_readable, session_on_writable, session_on_error };
    return ntk_socket_attach_poller(session->sock, poller, &cb, session);
}

static ntk_tcp_session *session_create(ntk_tcp_server *srv, ntk_socket *client, ntk_poller *poller)
{
    ntk_tcp_session *session = (ntk_tcp_session *)calloc(1, sizeof(*session));
    if (!session)
        return NULL;

    session->server = srv;
    session->sock = client;
    session->poller = poller;
    if (srv->session_create_user)
        session->user = srv->session_create_user(srv, session);
    else
        session->user = srv->session_user;

    if (session_attach(session, poller) != NTK_OK) {
        free(session);
        return NULL;
    }
    return session;
}

static ntk_err_t session_list_add(ntk_tcp_server *srv, ntk_tcp_session *session)
{
    ntk_mutex_lock(srv->mutex);
    if (srv->session_count >= srv->session_cap) {
        size_t cap = srv->session_cap ? srv->session_cap * 2 : 8;
        ntk_tcp_session **p = (ntk_tcp_session **)realloc(srv->sessions, cap * sizeof(*p));
        if (!p) {
            ntk_mutex_unlock(srv->mutex);
            return NTK_ERR_NOMEM;
        }
        srv->sessions = p;
        srv->session_cap = cap;
    }
    srv->sessions[srv->session_count++] = session;
    ntk_mutex_unlock(srv->mutex);
    return NTK_OK;
}

static void session_out_free_chunk(ntk_tcp_out_chunk *chunk)
{
    if (!chunk)
        return;
    if (chunk->kind == NTK_TCP_OUT_MEM)
        free(chunk->u.mem);
    else if (chunk->kind == NTK_TCP_OUT_BUF)
        ntk_buf_unref(chunk->u.buf);
    free(chunk);
}

static void session_out_pop_chunk(ntk_tcp_session *session)
{
    ntk_tcp_out_chunk *chunk = session->out_head;
    if (!chunk)
        return;
    session->out_head = chunk->next;
    if (!session->out_head)
        session->out_tail = NULL;
    if (session->out_pending >= chunk->len)
        session->out_pending -= chunk->len;
    else
        session->out_pending = 0;
    session_out_free_chunk(chunk);
}

static const void *session_chunk_ptr(const ntk_tcp_out_chunk *chunk)
{
    if (!chunk)
        return NULL;
    if (chunk->kind == NTK_TCP_OUT_BUF)
        return (const uint8_t *)ntk_buf_data(chunk->u.buf) + chunk->off;
    return chunk->u.mem + chunk->off;
}

static void session_out_check_highwater(ntk_tcp_session *session)
{
    ntk_tcp_server *srv;

    if (!session)
        return;
    srv = session->server;
    if (!srv || !srv->on_out_highwater || srv->out_highwater_bytes == 0)
        return;
    if (session->out_pending < srv->out_highwater_bytes) {
        session->out_highwater_hit = 0;
        return;
    }
    if (!session->out_highwater_hit) {
        session->out_highwater_hit = 1;
        srv->on_out_highwater(session, session->out_pending, session->user);
    }
}

static int session_out_append_buf(ntk_tcp_session *session, ntk_buf *buf);

static int session_out_enqueue(ntk_tcp_session *session, ntk_tcp_out_chunk *chunk)
{
    if (!session || !chunk)
        return -1;
    chunk->next = NULL;
    if (session->out_tail)
        session->out_tail->next = chunk;
    else
        session->out_head = chunk;
    session->out_tail = chunk;
    session->out_pending += chunk->len;
    session_out_check_highwater(session);
    return 0;
}

static int session_out_append_mem(ntk_tcp_session *session, const void *data, size_t len)
{
    ntk_buf *buf;
    int r;

    if (!session || !data || len == 0)
        return 0;
    /* 会话钉在 poller 上：出站拷贝走本地无锁池（无本地池则回退共享池/malloc） */
    buf = ntk_buf_alloc_local(session->poller, len);
    if (!buf)
        return -1;
    memcpy((void *)ntk_buf_data(buf), data, len);
    ntk_buf_set_len(buf, len);
#ifdef NTK_TEST_HOOKS
    g_ntk_test_out_mem_append += len;
#endif
    r = session_out_append_buf(session, buf);
    ntk_buf_unref(buf);
    return r;
}

static int session_out_append_buf(ntk_tcp_session *session, ntk_buf *buf)
{
    ntk_tcp_out_chunk *chunk;
    size_t len;

    if (!session || !buf)
        return -1;
    len = ntk_buf_len(buf);
    if (len == 0)
        return 0;
    chunk = (ntk_tcp_out_chunk *)calloc(1, sizeof(*chunk));
    if (!chunk)
        return -1;
    chunk->kind = NTK_TCP_OUT_BUF;
    chunk->off = 0;
    chunk->len = len;
    chunk->u.buf = ntk_buf_ref(buf);
    return session_out_enqueue(session, chunk);
}

static void session_out_advance_sent(ntk_tcp_session *session, size_t sent)
{
    size_t skip = sent;

    while (skip > 0 && session->out_head) {
        ntk_tcp_out_chunk *chunk = session->out_head;

        if (chunk->len == 0) {
            session_out_pop_chunk(session);
            continue;
        }
        if (skip >= chunk->len) {
            skip -= chunk->len;
            session_out_pop_chunk(session);
            continue;
        }
        chunk->off += skip;
        chunk->len -= skip;
        if (session->out_pending >= skip)
            session->out_pending -= skip;
        else
            session->out_pending = 0;
        skip = 0;
    }
}

static ntk_ssize_t session_out_try_sendv(ntk_tcp_session *session, unsigned *out_count)
{
    ntk_socket_iov iov[NTK_SOCKET_IOV_MAX];
    unsigned count = 0;
    size_t total = 0;
    ntk_tcp_out_chunk *c;

    if (!session || !session->sock)
        return NTK_ERR_INVALID;

    for (c = session->out_head; c && count < NTK_SOCKET_IOV_MAX; c = c->next) {
        const void *ptr;

        if (c->len == 0)
            continue;
        ptr = session_chunk_ptr(c);
        if (!ptr)
            continue;
        iov[count].base = ptr;
        iov[count].len = c->len;
        total += c->len;
        count++;
    }
    if (out_count)
        *out_count = count;
    if (count == 0)
        return 0;
    if (count == 1)
        return ntk_socket_send(session->sock, iov[0].base, iov[0].len);
    return ntk_socket_sendv(session->sock, iov, count);
}

static void session_out_flush(ntk_tcp_session *session)
{
    if (!session || !session->sock)
        return;

    while (session->out_head) {
        unsigned count = 0;
        size_t total = 0;
        ntk_tcp_out_chunk *c;
        ntk_ssize_t n;

        if (session->out_head->len == 0) {
            session_out_pop_chunk(session);
            continue;
        }

        for (c = session->out_head; c && count < NTK_SOCKET_IOV_MAX; c = c->next) {
            if (c->len == 0)
                break;
            total += c->len;
            count++;
        }
        if (count == 0)
            break;

        n = session_out_try_sendv(session, &count);
        if (n == (ntk_ssize_t)total) {
            session_out_advance_sent(session, (size_t)n);
            continue;
        }
        if (n > 0) {
            session_out_advance_sent(session, (size_t)n);
            continue;
        }
        break;
    }
    session_update_out_events(session);
}

static void session_destroy(ntk_tcp_session *session)
{
    if (!session)
        return;
    session_detach(session);
    while (session->out_head)
        session_out_pop_chunk(session);
    ntk_socket_destroy(session->sock);
    free(session);
}

static void session_remove(ntk_tcp_server *srv, ntk_tcp_session *target)
{
    if (!srv || !target || target->closing)
        return;
    target->closing = 1;

    ntk_mutex_lock(srv->mutex);
    for (size_t i = 0; i < srv->session_count; ++i) {
        if (srv->sessions[i] == target) {
            srv->sessions[i] = srv->sessions[srv->session_count - 1];
            srv->session_count--;
            break;
        }
    }
    ntk_mutex_unlock(srv->mutex);

    if (srv->ops.on_error)
        srv->ops.on_error(target, target->user);

    session_destroy(target);
}

static void session_detach(ntk_tcp_session *session)
{
    if (!session || !session->sock)
        return;
    ntk_socket_detach_poller(session->sock);
}

static ntk_poller *choose_poller(ntk_tcp_server *srv, ntk_poller *accept_poller)
{
    if (srv->pick_poller) {
        ntk_poller *p = srv->pick_poller(srv, accept_poller, srv->pick_poller_user);
        if (p)
            return p;
    }
    if (srv->pool_pick_poller && srv->poller_pool) {
        ntk_poller *p = ntk_tcp_pick_poller_from_pool(srv->poller_pool, accept_poller);
        if (p)
            return p;
    }
    return accept_poller;
}

static void listen_on_readable(int fd, unsigned events, void *user)
{
    ntk_listen_binding *bind = (ntk_listen_binding *)user;
    ntk_tcp_server *srv = bind->server;
    ntk_poller *accept_poller = bind->poller;
    (void)events;

    if (!srv || !srv->listen_sock || ntk_socket_fd(srv->listen_sock) != fd)
        return;

    for (;;) {
        ntk_socket *client = NULL;
        ntk_err_t err = ntk_socket_accept(srv->listen_sock, &client);
        if (err == NTK_ERR_AGAIN)
            break;
        if (err != NTK_OK || !client)
            break;

        ntk_poller *target = choose_poller(srv, accept_poller);
        ntk_tcp_session *session = session_create(srv, client, target);
        if (!session) {
            ntk_socket_destroy(client);
            continue;
        }
        if (session_list_add(srv, session) != NTK_OK) {
            session_destroy(session);
            continue;
        }
    }
}

static void run_manager_on_poller(ntk_tcp_server *srv, ntk_poller *poller)
{
    ntk_mutex_lock(srv->mutex);
    for (size_t i = 0; i < srv->session_count; ++i) {
        ntk_tcp_session *s = srv->sessions[i];
        if (!s || s->closing || s->poller != poller)
            continue;
        session_out_flush(s);
        ntk_mutex_unlock(srv->mutex);
        if (srv->ops.on_manager)
            srv->ops.on_manager(s, s->user);
        ntk_mutex_lock(srv->mutex);
    }
    ntk_mutex_unlock(srv->mutex);
}

static uint64_t manager_timer_cb(void *user)
{
    ntk_listen_binding *bind = (ntk_listen_binding *)user;
    ntk_tcp_server *srv = bind->server;
    run_manager_on_poller(srv, bind->poller);
    if (srv->manager_interval_sec <= 0)
        return 0;
    return (uint64_t)(srv->manager_interval_sec * NTK_MS_PER_SEC);
}

static ntk_err_t listen_attach_manager(ntk_tcp_server *srv, ntk_listen_binding *bind)
{
    if (srv->manager_interval_sec <= 0 || !srv->ops.on_manager)
        return NTK_OK;
    bind->manager_timer = ntk_poller_do_delay(bind->poller,
        (uint64_t)(srv->manager_interval_sec * NTK_MS_PER_SEC), manager_timer_cb, bind);
    if (!bind->manager_timer)
        return NTK_ERR_NOMEM;
    return NTK_OK;
}

ntk_tcp_server *ntk_tcp_server_create(const ntk_tcp_server_opts_t *opts)
{
    if (!opts || !opts->session_ops || !opts->poller_pool)
        return NULL;

    unsigned n = ntk_poller_pool_size(opts->poller_pool);
    if (n == 0)
        return NULL;

    ntk_tcp_server *srv = (ntk_tcp_server *)calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;

    srv->mutex = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!srv->mutex) {
        free(srv);
        return NULL;
    }

    srv->poller_count = n;
    srv->pollers = (ntk_poller **)calloc(n, sizeof(ntk_poller *));
    if (!srv->pollers) {
        ntk_tcp_server_destroy(srv);
        return NULL;
    }

    for (unsigned i = 0; i < n; ++i) {
        srv->pollers[i] = ntk_poller_pool_at(opts->poller_pool, i);
        if (!srv->pollers[i]) {
            ntk_tcp_server_destroy(srv);
            return NULL;
        }
    }

    srv->ops = *opts->session_ops;
    srv->session_user = opts->session_user;
    srv->session_create_user = opts->session_create_user;
    srv->poller_pool = opts->poller_pool;
    srv->pick_poller = opts->pick_poller;
    srv->pick_poller_user = opts->pick_poller_user;
    if (srv->poller_pool && !srv->pick_poller)
        srv->pool_pick_poller = 1;

    if (opts->manager_interval_sec < 0)
        srv->manager_interval_sec = -1.0f;
    else if (opts->manager_interval_sec > 0)
        srv->manager_interval_sec = opts->manager_interval_sec;
    else if (opts->session_ops->on_manager)
        srv->manager_interval_sec = NTK_TCP_DEFAULT_MANAGER_SEC;
    else
        srv->manager_interval_sec = -1.0f;

    srv->out_highwater_bytes = opts->out_highwater_bytes;
    srv->on_out_highwater = opts->on_out_highwater;

    if (opts->host)
        strncpy(srv->host, opts->host, sizeof(srv->host) - 1);
    else
        srv->host[0] = '\0';
    srv->port = opts->port;
    srv->backlog = opts->backlog > 0 ? opts->backlog : NTK_TCP_DEFAULT_BACKLOG;

    srv->listen_sock = ntk_socket_create();
    if (!srv->listen_sock) {
        ntk_tcp_server_destroy(srv);
        return NULL;
    }

    srv->listen_bindings = (ntk_listen_binding *)calloc(n, sizeof(*srv->listen_bindings));
    if (!srv->listen_bindings) {
        ntk_tcp_server_destroy(srv);
        return NULL;
    }
    return srv;
}

void ntk_tcp_server_destroy(ntk_tcp_server *srv)
{
    if (!srv)
        return;
    ntk_tcp_server_stop(srv);

    ntk_mutex_lock(srv->mutex);
    for (size_t i = 0; i < srv->session_count; ++i)
        session_destroy(srv->sessions[i]);
    srv->session_count = 0;
    ntk_mutex_unlock(srv->mutex);

    free(srv->sessions);
    free(srv->listen_bindings);
    free(srv->pollers);
    ntk_socket_destroy(srv->listen_sock);
    ntk_mutex_destroy(srv->mutex);
    free(srv);
}

ntk_err_t ntk_tcp_server_start(ntk_tcp_server *srv)
{
    if (!srv || srv->started)
        return NTK_ERR_STATE;

    ntk_err_t err = ntk_socket_listen(srv->listen_sock, srv->host[0] ? srv->host : NULL, srv->port, srv->backlog);
    if (err != NTK_OK)
        return err;

    char ip[NTK_NET_ADDR_STR_LEN];
    uint16_t bound_port = 0;
    if (ntk_socket_get_local(srv->listen_sock, ip, sizeof(ip), &bound_port) == NTK_OK && bound_port)
        srv->port = bound_port;

    int listen_fd = ntk_socket_fd(srv->listen_sock);
    for (unsigned i = 0; i < srv->poller_count; ++i) {
        srv->listen_bindings[i].server = srv;
        srv->listen_bindings[i].poller = srv->pollers[i];
        err = ntk_poller_add(srv->pollers[i], listen_fd, NTK_POLL_IN | NTK_POLL_ERR | NTK_POLL_HUP,
                              listen_on_readable, &srv->listen_bindings[i]);
        if (err != NTK_OK) {
            ntk_tcp_server_stop(srv);
            return err;
        }
        err = listen_attach_manager(srv, &srv->listen_bindings[i]);
        if (err != NTK_OK) {
            ntk_tcp_server_stop(srv);
            return err;
        }
    }

    srv->started = 1;
    return NTK_OK;
}

void ntk_tcp_server_stop(ntk_tcp_server *srv)
{
    if (!srv || !srv->started)
        return;

    int listen_fd = ntk_socket_fd(srv->listen_sock);
    for (unsigned i = 0; i < srv->poller_count; ++i) {
        if (srv->listen_bindings[i].manager_timer) {
            ntk_poller_timer_cancel(srv->listen_bindings[i].manager_timer);
            srv->listen_bindings[i].manager_timer = NULL;
        }
        ntk_poller_del(srv->pollers[i], listen_fd);
    }

    srv->started = 0;
}

int ntk_tcp_server_is_running(const ntk_tcp_server *srv)
{
    return srv && srv->started;
}

uint16_t ntk_tcp_server_port(const ntk_tcp_server *srv)
{
    return srv ? srv->port : 0;
}

unsigned ntk_tcp_server_poller_count(const ntk_tcp_server *srv)
{
    return srv ? srv->poller_count : 0;
}

ntk_poller *ntk_tcp_server_poller(const ntk_tcp_server *srv, unsigned index)
{
    if (!srv || index >= srv->poller_count)
        return NULL;
    return srv->pollers[index];
}

ntk_socket *ntk_tcp_session_socket(ntk_tcp_session *session)
{
    return session ? session->sock : NULL;
}

void *ntk_tcp_session_user(ntk_tcp_session *session)
{
    return session ? session->user : NULL;
}

ntk_poller *ntk_tcp_session_poller(ntk_tcp_session *session)
{
    return session ? session->poller : NULL;
}

ntk_err_t ntk_tcp_session_send(ntk_tcp_session *session, const void *data, size_t len)
{
    if (!session || session->closing || !data || len == 0)
        return NTK_ERR_INVALID;

    if (session->out_head) {
        if (session_out_append_mem(session, data, len) != 0)
            return NTK_ERR_NOMEM;
        session_out_flush(session);
        return NTK_OK;
    }

    {
        ntk_ssize_t n = ntk_socket_send(session->sock, data, len);
        if (n == (ntk_ssize_t)len)
            return NTK_OK;
        if (n > 0) {
            if (session_out_append_mem(session, (const uint8_t *)data + n, len - (size_t)n) != 0)
                return NTK_ERR_NOMEM;
            session_out_flush(session);
            return NTK_OK;
        }
        if (n == NTK_ERR_AGAIN) {
            if (session_out_append_mem(session, data, len) != 0)
                return NTK_ERR_NOMEM;
            return NTK_OK;
        }
        return (ntk_err_t)n;
    }
}

ntk_err_t ntk_tcp_session_sendv(ntk_tcp_session *session,
                                const void *parts[], const size_t lens[], unsigned count)
{
    ntk_socket_iov iov[NTK_SOCKET_IOV_MAX];
    unsigned i;
    size_t total = 0;

    if (!session || session->closing || !parts || !lens || count == 0)
        return NTK_ERR_INVALID;
    if (count > NTK_SOCKET_IOV_MAX)
        count = NTK_SOCKET_IOV_MAX;

    for (i = 0; i < count; ++i) {
        if (!parts[i] || lens[i] == 0)
            return NTK_ERR_INVALID;
        iov[i].base = parts[i];
        iov[i].len = lens[i];
        total += lens[i];
    }

    if (session->out_head) {
        for (i = 0; i < count; ++i) {
            if (session_out_append_mem(session, parts[i], lens[i]) != 0)
                return NTK_ERR_NOMEM;
        }
        session_out_flush(session);
        return NTK_OK;
    }

    {
        ntk_ssize_t n = ntk_socket_sendv(session->sock, iov, count);
        if (n == (ntk_ssize_t)total)
            return NTK_OK;
        if (n > 0) {
            size_t skip = (size_t)n;
            for (i = 0; i < count; ++i) {
                if (skip >= lens[i]) {
                    skip -= lens[i];
                    continue;
                }
                if (session_out_append_mem(session, (const uint8_t *)parts[i] + skip, lens[i] - skip) != 0)
                    return NTK_ERR_NOMEM;
                skip = 0;
                for (++i; i < count; ++i) {
                    if (session_out_append_mem(session, parts[i], lens[i]) != 0)
                        return NTK_ERR_NOMEM;
                }
                break;
            }
            session_out_flush(session);
            return NTK_OK;
        }
        if (n == NTK_ERR_AGAIN) {
            for (i = 0; i < count; ++i) {
                if (session_out_append_mem(session, parts[i], lens[i]) != 0)
                    return NTK_ERR_NOMEM;
            }
            return NTK_OK;
        }
        return (ntk_err_t)n;
    }
}

ntk_err_t ntk_tcp_session_send_buf(ntk_tcp_session *session, ntk_buf *buf)
{
    size_t len;
    const void *data;

    if (!session || session->closing || !buf)
        return NTK_ERR_INVALID;
    len = ntk_buf_len(buf);
    if (len == 0)
        return NTK_OK;
    data = ntk_buf_data(buf);
    if (!data)
        return NTK_ERR_INVALID;

    if (session->out_head) {
        if (session_out_append_buf(session, buf) != 0)
            return NTK_ERR_NOMEM;
        session_out_flush(session);
        return NTK_OK;
    }

    {
        ntk_ssize_t n = ntk_socket_send(session->sock, data, len);
        if (n == (ntk_ssize_t)len)
            return NTK_OK;
        if (n > 0) {
            ntk_tcp_out_chunk *chunk;
            chunk = (ntk_tcp_out_chunk *)calloc(1, sizeof(*chunk));
            if (!chunk)
                return NTK_ERR_NOMEM;
            chunk->kind = NTK_TCP_OUT_BUF;
            chunk->off = (size_t)n;
            chunk->len = len - (size_t)n;
            chunk->u.buf = ntk_buf_ref(buf);
            if (session_out_enqueue(session, chunk) != 0) {
                ntk_buf_unref(chunk->u.buf);
                free(chunk);
                return NTK_ERR_NOMEM;
            }
            session_out_flush(session);
            return NTK_OK;
        }
        if (n == NTK_ERR_AGAIN) {
            if (session_out_append_buf(session, buf) != 0)
                return NTK_ERR_NOMEM;
            return NTK_OK;
        }
        return (ntk_err_t)n;
    }
}

void ntk_tcp_session_flush(ntk_tcp_session *session)
{
    session_out_flush(session);
}

size_t ntk_tcp_session_out_pending(const ntk_tcp_session *session)
{
    return session ? session->out_pending : 0;
}

void ntk_tcp_session_out_discard(ntk_tcp_session *session)
{
    if (!session)
        return;
    while (session->out_head)
        session_out_pop_chunk(session);
    session->out_highwater_hit = 0;
}

void ntk_tcp_session_close(ntk_tcp_session *session)
{
    if (!session || !session->server)
        return;
    session_remove(session->server, session);
}

#ifdef NTK_TEST_HOOKS
void ntk_tcp_test_reset_out_mem_append(void)
{
    g_ntk_test_out_mem_append = 0;
}

size_t ntk_tcp_test_out_mem_append_bytes(void)
{
    return g_ntk_test_out_mem_append;
}
#endif
