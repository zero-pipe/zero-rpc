/*
 * TCP 传输驱动：基于 ntk_tcp_server / ntk_tcp_client 的长度前缀分帧。
 *
 * 线程模型：入站会话由 ntk_tcp_server 分散到 poller_pool 的各个 poller，出站连接
 * 创建在发起调用的 poller 上。为避免跨线程争用，link 表按 poller 分片（每个 poller 独占一行），
 * 热路径无锁；同一会话的 on_recv/on_error 始终在其所属 poller 上执行。
 *
 * 缓冲模型：
 *   - 发送快路径：header（栈上）+ payload（借用）一次 sendv 写出，零用户态拷贝。
 *     半写/积压时仅把“未写后缀”拷进 nb_iobuf 待写队列，由 tick / 可写事件续写，
 *     不再有 body+frame 两次 malloc 与整段 memcpy。
 *   - 接收：per-link 池化连续累积缓冲 + 读游标；帧到齐后原地解码，均摊压实，
 *     避免逐帧 memmove 与 realloc。
 *
 * 无分片/重传：TCP 自身可靠有序。
 */
#include "transport_internal.h"

#include "atomic.h"
#include "envelope.h"
#include "iobuf.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* 入站累积：池化连续缓冲 + 读游标，均摊压实。 */
typedef struct nb_rx_acc {
    uint8_t *buf;
    size_t cap;
    size_t rpos;        /* 已消费 */
    size_t wpos;        /* 已写入 */
    ntk_buf_pool *pool; /* buf 所属池；跨线程释放不能从 current poller 推断 */
} nb_rx_acc_t;

/* 一条 TCP 连接：入站（服务端会话）或出站（客户端连接）。 */
typedef struct nb_tcp_link {
    int used;
    int server_side;
    char ip[NB_ENDPOINT_MAX];
    uint16_t port;
    ntk_tcp_session *session; /* 服务端侧：accept 得到 */
    ntk_tcp_client *client;   /* 客户端侧：connect 发起 */
    int connected;
    nb_rx_acc_t rx; /* 接收累积 */
    nb_iobuf_t tx;  /* 出站待写队列（仅客户端侧使用） */
} nb_tcp_link_t;

typedef struct nb_tcp_transport {
    nb_transport_t base;
    ntk_tcp_server *server;
    ntk_poller *pollers[IO_MAX];
    int io_count;
    nb_tcp_link_t links[IO_MAX][TCP_LINKS_PER_IO];
    ntk_timer *tick_timers[IO_MAX];
    struct nb_tcp_io_ctx *io_ctx;
} nb_tcp_transport_t;

typedef struct nb_tcp_io_ctx {
    nb_tcp_transport_t *t;
    int io;
} nb_tcp_io_ctx_t;

static void nb_tcp_client_on_connect(ntk_tcp_client *client, void *user);
static void nb_tcp_client_on_recv(ntk_tcp_client *client, const void *data, size_t len, void *user);
static void nb_tcp_client_on_error(ntk_tcp_client *client, void *user);
static void nb_tcp_tick_cb(void *user);

/* ---- 接收累积 ---- */

static void nb_rx_acc_init(nb_rx_acc_t *a)
{
    a->buf = NULL;
    a->cap = 0;
    a->rpos = 0;
    a->wpos = 0;
    a->pool = NULL;
}

static void nb_rx_acc_free(nb_rx_acc_t *a)
{
    if (!a || !a->buf) {
        return;
    }
    if (a->pool) {
        ntk_buf_pool_release(a->pool, a->buf, a->cap);
    } else {
        free(a->buf);
    }
    nb_rx_acc_init(a);
}

static int nb_rx_acc_reserve(nb_rx_acc_t *a, size_t extra)
{
    ntk_poller *poller;
    ntk_buf_pool *pool;
    size_t used = a->wpos - a->rpos;

    if (extra == 0) {
        return NB_OK;
    }
    if (a->cap - a->wpos >= extra) {
        return NB_OK;
    }
    /* 尾部空间不足：先均摊压实（把未消费数据搬到头部），再决定是否扩容。 */
    if (a->rpos > 0) {
        memmove(a->buf, a->buf + a->rpos, used);
        a->rpos = 0;
        a->wpos = used;
        if (a->cap - a->wpos >= extra) {
            return NB_OK;
        }
    }
    poller = ntk_poller_current();
    pool = a->pool ? a->pool : (poller ? ntk_poller_buf_pool(poller) : NULL);
    {
        size_t need = a->wpos + extra;
        size_t ncap = a->cap ? a->cap : 8192;
        while (ncap < need) {
            ncap *= 2;
        }
        if (pool) {
            size_t got = 0;
            uint8_t *nb = (uint8_t *)ntk_buf_pool_acquire(pool, ncap, &got);
            if (!nb) {
                return NB_ERR_NOMEM;
            }
            if (a->buf) {
                memcpy(nb, a->buf, used);
                if (a->pool) {
                    ntk_buf_pool_release(a->pool, a->buf, a->cap);
                } else {
                    free(a->buf);
                }
            }
            a->buf = nb;
            a->cap = got;
            a->pool = pool;
        } else {
            uint8_t *nb = (uint8_t *)realloc(a->buf, ncap);
            if (!nb) {
                return NB_ERR_NOMEM;
            }
            a->buf = nb;
            a->cap = ncap;
        }
        a->rpos = 0;
        a->wpos = used;
    }
    return NB_OK;
}

/* ---- 解析并投递完整帧 ---- */

static void nb_tcp_link_dispatch(nb_tcp_transport_t *t,
                              nb_tcp_link_t *link,
                              const uint8_t *body,
                              size_t body_len)
{
    nb_envelope_t env;
    nb_message_t msg;
    char route[MAX_ROUTE];

    if (nb_envelope_decode(body, body_len, &env) != NB_OK) {
        return;
    }
    nb_envelope_to_message(&env, route, sizeof(route), &msg);
    msg.peer_ip = link->ip;
    msg.peer_port = link->port;
    msg.peer_token = link;
    nb_node_on_message(t->base.node, &t->base, &msg);
}

static void nb_tcp_link_consume(nb_tcp_transport_t *t, nb_tcp_link_t *link)
{
    size_t max_frame = (size_t)t->base.max_msg_bytes + MAX_ENVELOPE_EXTRA;
    nb_rx_acc_t *a = &link->rx;

    for (;;) {
        size_t avail = a->wpos - a->rpos;
        const uint8_t *body = NULL;
        size_t body_len = 0;
        size_t frame;

        if (avail < TCP_HDR_LEN) {
            return;
        }
        frame = nb_tcp_frame_size(a->buf + a->rpos, avail);
        if (frame == 0 || frame > max_frame) {
            a->rpos = a->wpos = 0; /* 协议错误：丢弃累积 */
            return;
        }
        if (avail < frame) {
            return;
        }
        if (nb_tcp_parse(a->buf + a->rpos, frame, &body, &body_len) == 1) {
            NB_METRIC_INC(t->base.metrics->tcp_frames_received);
            nb_tcp_link_dispatch(t, link, body, body_len);
        }
        a->rpos += frame;
        if (a->rpos == a->wpos) {
            a->rpos = a->wpos = 0;
        }
    }
}

static void nb_tcp_link_on_bytes(nb_tcp_transport_t *t,
                              nb_tcp_link_t *link,
                              const void *data,
                              size_t len)
{
    if (!link || !data || len == 0) {
        return;
    }
    if (nb_rx_acc_reserve(&link->rx, len) != NB_OK) {
        return;
    }
    memcpy(link->rx.buf + link->rx.wpos, data, len);
    link->rx.wpos += len;
    nb_tcp_link_consume(t, link);
}

/* ---- 出站发送 ---- */

static void nb_tcp_link_flush_tx(nb_tcp_transport_t *t, nb_tcp_link_t *link)
{
    (void)t;
    if (link->server_side || !link->connected || !link->client) {
        return;
    }
    while (!nb_iobuf_empty(&link->tx)) {
        ntk_socket_iov iov[NTK_SOCKET_IOV_MAX];
        unsigned n = nb_iobuf_iovec(&link->tx, iov, NTK_SOCKET_IOV_MAX, 0);
        ntk_socket *sock;
        ntk_ssize_t sent;

        if (n == 0) {
            break;
        }
        sock = ntk_tcp_client_socket(link->client);
        if (!sock) {
            break;
        }
        sent = ntk_socket_sendv(sock, iov, n);
        if (sent > 0) {
            nb_iobuf_consume(&link->tx, (size_t)sent);
            continue;
        }
        break; /* EAGAIN / 错误：保留队列，交由 tick 或可写事件续写 */
    }
}

/*
 * 发送一帧。快路径直接 sendv(header, payload)，零用户态拷贝；
 * 半写/积压时只把未写后缀拷进待写队列（一次拷贝），随后尽力冲刷。
 */
static int nb_tcp_link_emit(nb_tcp_transport_t *t,
                         nb_tcp_link_t *link,
                         const uint8_t *header,
                         size_t header_len,
                         const void *payload,
                         size_t payload_len)
{
    ntk_poller *poller = ntk_poller_current();
    size_t total = header_len + payload_len;

    if (!t || !link || !link->used || !header || header_len == 0) {
        return NB_ERR_INVALID;
    }

    /* 入站会话自带出站队列（ntk_buf 引用），直接委托，避免二次排队。 */
    if (link->server_side) {
        const void *parts[2];
        size_t lens[2];
        ntk_err_t rc;
        if (!link->session) {
            return NB_ERR_STATE;
        }
        parts[0] = header;
        lens[0] = header_len;
        parts[1] = payload;
        lens[1] = payload_len;
        rc = ntk_tcp_session_sendv(link->session, parts, lens, payload_len ? 2u : 1u);
        if (rc == NTK_OK) {
            NB_METRIC_INC(t->base.metrics->tcp_frames_sent);
            return NB_OK;
        }
        return (rc == NTK_ERR_AGAIN) ? NB_ERR_AGAIN : NB_ERR_IO;
    }

    if (!link->client) {
        return NB_ERR_AGAIN;
    }
    if (!link->connected) {
        /* 异步 connect 尚未完成：保留整帧，on_connect 后由 flush_tx 发出。 */
        if (nb_iobuf_append_copy(&link->tx, poller, header, header_len) != 0) {
            return NB_ERR_NOMEM;
        }
        if (payload_len && nb_iobuf_append_copy(&link->tx, poller, payload, payload_len) != 0) {
            return NB_ERR_NOMEM;
        }
        return NB_OK;
    }

    /* 已有积压：整帧入队（拷贝），冲刷由队列驱动。 */
    if (!nb_iobuf_empty(&link->tx)) {
        if (nb_iobuf_append_copy(&link->tx, poller, header, header_len) != 0) {
            return NB_ERR_NOMEM;
        }
        if (payload_len && nb_iobuf_append_copy(&link->tx, poller, payload, payload_len) != 0) {
            return NB_ERR_NOMEM;
        }
        nb_tcp_link_flush_tx(t, link);
        NB_METRIC_INC(t->base.metrics->tcp_frames_sent);
        return NB_OK;
    }

    /* 快路径：一次 sendv / send 写出 header + payload。 */
    {
        ntk_socket *sock = ntk_tcp_client_socket(link->client);
        ntk_ssize_t sent;
        if (!sock) {
            return NB_ERR_IO;
        }
        if (payload_len) {
            ntk_socket_iov iov[2];
            iov[0].base = header;
            iov[0].len = header_len;
            iov[1].base = payload;
            iov[1].len = payload_len;
            sent = ntk_socket_sendv(sock, iov, 2);
        } else {
            sent = ntk_socket_send(sock, header, header_len);
        }
        if (sent == (ntk_ssize_t)total) {
            NB_METRIC_INC(t->base.metrics->tcp_frames_sent);
            return NB_OK;
        }
        if (sent == NTK_ERR_AGAIN) {
            sent = 0;
        } else if (sent < 0) {
            return NB_ERR_IO;
        }
        /* 半写：只拷贝未写后缀（一次拷贝，替代原先 malloc+两次 memcpy）。 */
        if ((size_t)sent < header_len) {
            size_t hrem = header_len - (size_t)sent;
            if (nb_iobuf_append_copy(&link->tx, poller, header + sent, hrem) != 0) {
                return NB_ERR_NOMEM;
            }
            if (payload_len &&
                nb_iobuf_append_copy(&link->tx, poller, payload, payload_len) != 0) {
                return NB_ERR_NOMEM;
            }
        } else if (payload_len) {
            size_t off = (size_t)sent - header_len;
            if (nb_iobuf_append_copy(
                    &link->tx, poller, (const uint8_t *)payload + off, payload_len - off) != 0) {
                return NB_ERR_NOMEM;
            }
        }
        nb_tcp_link_flush_tx(t, link);
        NB_METRIC_INC(t->base.metrics->tcp_frames_sent);
        return NB_OK;
    }
}

/* ---- 连接查找（各自 poller 独占一行，无锁） ---- */

static int nb_io_of_poller(nb_tcp_transport_t *t, ntk_poller *poller)
{
    int i;
    if (!poller) {
        return -1;
    }
    for (i = 0; i < t->io_count; i++) {
        if (t->pollers[i] == poller) {
            return i;
        }
    }
    return -1;
}

static nb_tcp_link_t *nb_link_find_out(nb_tcp_transport_t *t,
                                     const char *ip,
                                     uint16_t port,
                                     int create)
{
    int io = nb_io_of_poller(t, ntk_poller_current());
    nb_tcp_link_t *row;
    ntk_poller *poller;
    int i;
    int free_slot = -1;
    if (io < 0) {
        io = 0;
    }
    row = t->links[io];
    poller = (io < t->io_count) ? t->pollers[io] : t->base.poller;
    for (i = 0; i < TCP_LINKS_PER_IO; i++) {
        nb_tcp_link_t *link = &row[i];
        if (link->used && !link->server_side && link->port == port && strcmp(link->ip, ip) == 0) {
            return link;
        }
        if (!link->used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (!create || free_slot < 0) {
        return NULL;
    }
    {
        nb_tcp_link_t *link = &row[free_slot];
        ntk_tcp_client_ops_t ops;
        ntk_tcp_client_opts_t copts;
        memset(link, 0, sizeof(*link));
        link->used = 1;
        link->server_side = 0;
        if (t->base.metrics) {
            NB_METRIC_INC(t->base.metrics->connections);
        }
        nb_rx_acc_init(&link->rx);
        nb_iobuf_init(&link->tx);
        nb_copy_str(link->ip, sizeof(link->ip), ip, "0.0.0.0");
        link->port = port;

        memset(&ops, 0, sizeof(ops));
        ops.on_connect = nb_tcp_client_on_connect;
        ops.on_recv = nb_tcp_client_on_recv;
        ops.on_error = nb_tcp_client_on_error;
        memset(&copts, 0, sizeof(copts));
        copts.poller = poller;
        copts.ops = &ops;
        copts.user = t;
        link->client = ntk_tcp_client_create(&copts);
        if (!link->client) {
            if (t->base.metrics) {
                NB_METRIC_SUB(t->base.metrics->connections, 1u);
            }
            link->used = 0;
            return NULL;
        }
        if (ntk_tcp_client_connect(link->client, link->ip, link->port) != NTK_OK) {
            ntk_tcp_client_destroy(link->client);
            link->client = NULL;
            if (t->base.metrics) {
                NB_METRIC_SUB(t->base.metrics->connections, 1u);
            }
            link->used = 0;
            return NULL;
        }
        return link;
    }
}

static nb_tcp_link_t *nb_link_for_session(nb_tcp_transport_t *t,
                                        ntk_tcp_session *session,
                                        int create)
{
    int io = nb_io_of_poller(t, ntk_tcp_session_poller(session));
    nb_tcp_link_t *row;
    int i;
    int free_slot = -1;
    if (io < 0) {
        io = 0;
    }
    row = t->links[io];
    for (i = 0; i < TCP_LINKS_PER_IO; i++) {
        nb_tcp_link_t *link = &row[i];
        if (link->used && link->server_side && link->session == session) {
            return link;
        }
        if (!link->used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (!create || free_slot < 0) {
        return NULL;
    }
    {
        nb_tcp_link_t *link = &row[free_slot];
        ntk_socket *sock;
        char ip[NB_ENDPOINT_MAX];
        uint16_t port = 0;
        memset(link, 0, sizeof(*link));
        link->used = 1;
        link->server_side = 1;
        if (t->base.metrics) {
            NB_METRIC_INC(t->base.metrics->connections);
        }
        link->connected = 1;
        link->session = session;
        nb_rx_acc_init(&link->rx);
        nb_iobuf_init(&link->tx);
        sock = ntk_tcp_session_socket(session);
        if (sock && ntk_socket_get_peer(sock, ip, sizeof(ip), &port) == NTK_OK) {
            nb_copy_str(link->ip, sizeof(link->ip), ip, "0.0.0.0");
            link->port = port;
        } else {
            nb_copy_str(link->ip, sizeof(link->ip), "0.0.0.0", "0.0.0.0");
        }
        return link;
    }
}

static nb_tcp_link_t *nb_link_for_client(nb_tcp_transport_t *t, ntk_tcp_client *client)
{
    int io;
    for (io = 0; io < t->io_count; io++) {
        nb_tcp_link_t *row = t->links[io];
        int i;
        for (i = 0; i < TCP_LINKS_PER_IO; i++) {
            if (row[i].used && !row[i].server_side && row[i].client == client) {
                return &row[i];
            }
        }
    }
    return NULL;
}

/* ---- 服务端会话回调 ---- */

static void nb_tcp_srv_on_recv(ntk_tcp_session *session, const void *data, size_t len, void *user)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)user;
    nb_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = nb_link_for_session(t, session, 1);
    if (link) {
        nb_tcp_link_on_bytes(t, link, data, len);
    }
}

static void nb_tcp_srv_on_error(ntk_tcp_session *session, void *user)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)user;
    nb_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = nb_link_for_session(t, session, 0);
    if (!link) {
        return;
    }
    nb_rx_acc_free(&link->rx);
    nb_iobuf_reset(&link->tx);
    if (t->base.metrics) {
        NB_METRIC_SUB(t->base.metrics->connections, 1u);
    }
    link->used = 0;
}

/* ---- 客户端回调 ---- */

static void nb_tcp_client_on_connect(ntk_tcp_client *client, void *user)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)user;
    nb_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = nb_link_for_client(t, client);
    if (link) {
        link->connected = 1;
        nb_tcp_link_flush_tx(t, link);
    }
}

static void nb_tcp_client_on_recv(ntk_tcp_client *client, const void *data, size_t len, void *user)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)user;
    nb_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = nb_link_for_client(t, client);
    if (link) {
        nb_tcp_link_on_bytes(t, link, data, len);
    }
}

static void nb_tcp_client_on_error(ntk_tcp_client *client, void *user)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)user;
    nb_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = nb_link_for_client(t, client);
    if (!link) {
        return;
    }
    nb_rx_acc_free(&link->rx);
    nb_iobuf_reset(&link->tx);
    ntk_tcp_client_destroy(client);
    link->client = NULL;
    if (t->base.metrics) {
        NB_METRIC_SUB(t->base.metrics->connections, 1u);
    }
    link->used = 0;
}

/* ---- 传输接口 ---- */

static int nb_tcp_start(nb_transport_t *base)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)base;
    ntk_tcp_server_opts_t opts;
    ntk_tcp_session_ops_t sops;
    unsigned n;
    unsigned i;

    n = ntk_poller_pool_size(base->pool);
    if (n > IO_MAX) {
        n = IO_MAX;
    }
    t->io_count = (int)n;
    for (i = 0; i < n; i++) {
        t->pollers[i] = ntk_poller_pool_at(base->pool, i);
    }
    t->io_ctx = (nb_tcp_io_ctx_t *)calloc(n ? n : 1, sizeof(*t->io_ctx));
    if (!t->io_ctx) {
        return NB_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        t->io_ctx[i].t = t;
        t->io_ctx[i].io = (int)i;
    }

    memset(&sops, 0, sizeof(sops));
    sops.on_recv = nb_tcp_srv_on_recv;
    sops.on_error = nb_tcp_srv_on_error;
    memset(&opts, 0, sizeof(opts));
    opts.host = base->bind_host;
    opts.port = base->port;
    opts.poller_pool = base->pool;
    opts.session_ops = &sops;
    opts.session_user = t;
    opts.session_create_user = NULL;
    opts.manager_interval_sec = 0.0f;

    t->server = ntk_tcp_server_create(&opts);
    if (!t->server) {
        return NB_ERR_IO;
    }
    if (ntk_tcp_server_start(t->server) != NTK_OK) {
        ntk_tcp_server_destroy(t->server);
        t->server = NULL;
        return NB_ERR_IO;
    }
    base->port = ntk_tcp_server_port(t->server);
    for (i = 0; i < n; i++) {
        t->tick_timers[i] =
            ntk_timer_start(t->pollers[i], nb_tick_interval_ms(), 1, nb_tcp_tick_cb, &t->io_ctx[i]);
    }
    return NB_OK;
}

static void nb_tcp_free_link(nb_tcp_transport_t *t, nb_tcp_link_t *link)
{
    if (!link || !link->used) {
        return;
    }
    nb_rx_acc_free(&link->rx);
    nb_iobuf_reset(&link->tx);
    if (!link->server_side && link->client) {
        ntk_tcp_client_destroy(link->client);
        link->client = NULL;
    }
    if (link->used && t->base.metrics) {
        NB_METRIC_SUB(t->base.metrics->connections, 1u);
    }
    link->used = 0;
}

static void nb_tcp_stop(nb_transport_t *base)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)base;
    int i, j;
    for (i = 0; i < IO_MAX; i++) {
        if (t->tick_timers[i]) {
            ntk_timer_stop(t->tick_timers[i]);
            t->tick_timers[i] = NULL;
        }
    }
    /* 先停 listen，再销毁 server（会 detach 并销毁所有入站会话），最后清 link 表。 */
    if (t->server) {
        ntk_tcp_server_stop(t->server);
        ntk_tcp_server_destroy(t->server);
        t->server = NULL;
    }
    for (i = 0; i < IO_MAX; i++) {
        for (j = 0; j < TCP_LINKS_PER_IO; j++) {
            nb_tcp_free_link(t, &t->links[i][j]);
        }
    }
    free(t->io_ctx);
    t->io_ctx = NULL;
}

static int nb_tcp_send_common(nb_tcp_transport_t *t, nb_tcp_link_t *link, const nb_message_t *msg)
{
    size_t route_len = (msg && msg->route) ? strlen(msg->route) : 0;
    size_t payload_len = msg ? msg->payload.len : 0;
    size_t body_len;
    size_t prefix_expect;
    size_t prefix_len;
    size_t need;
    nb_envelope_t header_envelope;

    if (!t || !msg) {
        return NB_ERR_INVALID;
    }
    memset(&header_envelope, 0, sizeof(header_envelope));
    header_envelope.kind = msg->kind;
    header_envelope.flags = msg->flags;
    header_envelope.status = msg->status;
    header_envelope.encoding = msg->payload.encoding;
    header_envelope.request_id = msg->request_id;
    header_envelope.timeout_ms = msg->timeout_ms;
    header_envelope.route = msg->route;
    header_envelope.metadata = msg->metadata;
    header_envelope.metadata_len = msg->metadata_len;
    header_envelope.status_msg = msg->status_msg;
    header_envelope.status_msg_len = msg->status_msg_len;
    if (msg->has_stream) {
        header_envelope.stream_flags = (uint8_t)(STREAM_FLAG_STREAM | msg->stream.flags);
        header_envelope.stream_id = msg->stream.stream_id;
        header_envelope.stream_offset = msg->stream.offset;
        header_envelope.stream_total_size = msg->stream.total_size;
    }
    header_envelope.payload = NULL;
    header_envelope.payload_len = payload_len;

    body_len = nb_envelope_size(&header_envelope);
    if (body_len == 0 || body_len > (size_t)t->base.max_msg_bytes + MAX_ENVELOPE_EXTRA) {
        return NB_ERR_TOOBIG;
    }
    prefix_expect = body_len - payload_len;
    need = TCP_HDR_LEN + body_len;
    if (!link) {
        link = nb_link_find_out(t, msg->peer_ip, msg->peer_port, 1);
        if (!link) {
            return NB_ERR_NOMEM;
        }
    }
    if (!nb_iobuf_empty(&link->tx) &&
        nb_iobuf_len(&link->tx) + need > (size_t)t->base.bp_high_water) {
        NB_METRIC_INC(t->base.metrics->backpressure_events);
        return NB_ERR_AGAIN;
    }

    if (route_len < MAX_ROUTE && prefix_expect <= ENVELOPE_HDR_LEN + MAX_ENVELOPE_EXTRA) {
        uint8_t header[TCP_HDR_LEN + ENVELOPE_HDR_LEN + MAX_ENVELOPE_EXTRA];

        prefix_len = nb_envelope_encode_prefix(
            header + TCP_HDR_LEN, sizeof(header) - TCP_HDR_LEN, &header_envelope);
        if (prefix_len == 0 || prefix_len != prefix_expect) {
            return NB_ERR_INVALID;
        }
        nb_put_u32(header, (uint32_t)body_len);
        return nb_tcp_link_emit(
            t, link, header, TCP_HDR_LEN + prefix_len, msg->payload.data, payload_len);
    }
    /* 超长 route（正常不会走到）：用池化 header，避免 body+frame 两次分配。 */
    {
        uint8_t *header = (uint8_t *)malloc(TCP_HDR_LEN + prefix_expect);
        int rc;
        if (!header) {
            return NB_ERR_NOMEM;
        }
        nb_put_u32(header, (uint32_t)body_len);
        prefix_len = nb_envelope_encode_prefix(
            header + TCP_HDR_LEN, prefix_expect, &header_envelope);
        if (prefix_len == 0 || prefix_len != prefix_expect) {
            free(header);
            return NB_ERR_INVALID;
        }
        rc = nb_tcp_link_emit(
            t, link, header, TCP_HDR_LEN + prefix_len, msg->payload.data, payload_len);
        free(header);
        return rc;
    }
}

static int nb_tcp_send(nb_transport_t *base, const nb_message_t *msg)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)base;
    if (!msg || !msg->peer_ip || !msg->peer_port) {
        return NB_ERR_INVALID;
    }
    return nb_tcp_send_common(t, NULL, msg);
}

static int nb_tcp_reply(nb_transport_t *base, const nb_message_t *msg)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)base;
    nb_tcp_link_t *link;
    if (!msg) {
        return NB_ERR_INVALID;
    }
    link = (nb_tcp_link_t *)msg->peer_token;
    if (!link || !link->used) {
        return nb_tcp_send(base, msg);
    }
    return nb_tcp_send_common(t, link, msg);
}

static void nb_tcp_tick_cb(void *user)
{
    nb_tcp_io_ctx_t *ctx = (nb_tcp_io_ctx_t *)user;
    nb_tcp_transport_t *t = ctx->t;
    nb_tcp_link_t *row = t->links[ctx->io]; /* 只冲刷本 poller 的出站连接 */
    int i;
    for (i = 0; i < TCP_LINKS_PER_IO; i++) {
        nb_tcp_link_t *link = &row[i];
        if (link->used && !link->server_side && link->connected && !nb_iobuf_empty(&link->tx)) {
            nb_tcp_link_flush_tx(t, link);
        }
    }
}

static const nb_transport_ops_t nb_g_tcp_ops = {
    nb_tcp_start, nb_tcp_stop, nb_tcp_send, nb_tcp_reply, NULL, /* TCP 不使用 RTP/RTCP，无按对端 RTCP 统计 */
};

nb_transport_t *nb_tcp_transport_create(void)
{
    nb_tcp_transport_t *t = (nb_tcp_transport_t *)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->base.ops = &nb_g_tcp_ops;
    t->base.kind = NB_TRANSPORT_TCP;
    nb_copy_str(t->base.scheme, sizeof(t->base.scheme), "tcp", "tcp");
    return &t->base;
}
