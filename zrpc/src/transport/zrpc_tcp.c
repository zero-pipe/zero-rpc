/*
 * TCP 传输驱动：基于 ztk_tcp_server / ztk_tcp_client 的长度前缀分帧。
 *
 * 线程模型：入站会话由 ztk_tcp_server 分散到 poller_pool 的各个 poller，出站连接
 * 固定在 io0。为避免跨线程争用，link 表按 poller 分片（每个 poller 独占一行），
 * 热路径无锁；同一会话的 on_recv/on_error 始终在其所属 poller 上执行。
 *
 * 缓冲模型：
 *   - 发送快路径：header（栈上）+ payload（借用）一次 sendv 写出，零用户态拷贝。
 *     半写/积压时仅把“未写后缀”拷进 zrpc_iobuf 待写队列，由 tick / 可写事件续写，
 *     不再有 body+frame 两次 malloc 与整段 memcpy。
 *   - 接收：per-link 池化连续累积缓冲 + 读游标；帧到齐后原地解码，均摊压实，
 *     避免逐帧 memmove 与 realloc。
 *
 * 无分片/重传：TCP 自身可靠有序。
 */
#include "zrpc_transport.h"

#include "zrpc_internal.h"
#include "zrpc_envelope.h"
#include "zrpc_iobuf.h"

#include <stdlib.h>
#include <string.h>

/* 入站累积：池化连续缓冲 + 读游标，均摊压实。 */
typedef struct zrpc_rx_acc {
    uint8_t *buf;
    size_t cap;
    size_t rpos; /* 已消费 */
    size_t wpos; /* 已写入 */
    ztk_buf_pool *pool; /* buf 所属池；跨线程释放不能从 current poller 推断 */
} zrpc_rx_acc_t;

/* 一条 TCP 连接：入站（服务端会话）或出站（客户端连接）。 */
typedef struct zrpc_tcp_link {
    int used;
    int inbound;
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port;
    ztk_tcp_session *session; /* inbound */
    ztk_tcp_client *client;   /* outbound */
    int connected;
    zrpc_rx_acc_t rx; /* 接收累积 */
    zrpc_iobuf_t tx;  /* 出站待写队列（仅 outbound 使用） */
} zrpc_tcp_link_t;

typedef struct zrpc_tcp_transport {
    zrpc_transport_t base;
    ztk_tcp_server *server;
    ztk_poller *pollers[ZRPC_IO_MAX];
    int io_count;
    zrpc_tcp_link_t links[ZRPC_IO_MAX][ZRPC_TCP_LINKS_PER_IO];
    ztk_timer *tick_timer;
} zrpc_tcp_transport_t;

static void tcp_client_on_connect(ztk_tcp_client *client, void *user);
static void tcp_client_on_recv(ztk_tcp_client *client, const void *data, size_t len, void *user);
static void tcp_client_on_error(ztk_tcp_client *client, void *user);
static void tcp_tick_cb(void *user);

/* ---- 接收累积 ---- */

static void rx_acc_init(zrpc_rx_acc_t *a) {
    a->buf = NULL;
    a->cap = 0;
    a->rpos = 0;
    a->wpos = 0;
    a->pool = NULL;
}

static void rx_acc_free(zrpc_rx_acc_t *a) {
    if (!a || !a->buf) {
        return;
    }
    if (a->pool) {
        ztk_buf_pool_release(a->pool, a->buf, a->cap);
    } else {
        free(a->buf);
    }
    rx_acc_init(a);
}

static int rx_acc_reserve(zrpc_rx_acc_t *a, size_t extra) {
    ztk_poller *poller;
    ztk_buf_pool *pool;
    size_t used = a->wpos - a->rpos;

    if (extra == 0) {
        return ZRPC_OK;
    }
    if (a->cap - a->wpos >= extra) {
        return ZRPC_OK;
    }
    /* 尾部空间不足：先均摊压实（把未消费数据搬到头部），再决定是否扩容。 */
    if (a->rpos > 0) {
        memmove(a->buf, a->buf + a->rpos, used);
        a->rpos = 0;
        a->wpos = used;
        if (a->cap - a->wpos >= extra) {
            return ZRPC_OK;
        }
    }
    poller = ztk_poller_current();
    pool = a->pool ? a->pool : (poller ? ztk_poller_buf_pool(poller) : NULL);
    {
        size_t need = a->wpos + extra;
        size_t ncap = a->cap ? a->cap : 8192;
        while (ncap < need) {
            ncap *= 2;
        }
        if (pool) {
            size_t got = 0;
            uint8_t *nb = (uint8_t *)ztk_buf_pool_acquire(pool, ncap, &got);
            if (!nb) {
                return ZRPC_ERR_NOMEM;
            }
            if (a->buf) {
                memcpy(nb, a->buf, used);
                if (a->pool) {
                    ztk_buf_pool_release(a->pool, a->buf, a->cap);
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
                return ZRPC_ERR_NOMEM;
            }
            a->buf = nb;
            a->cap = ncap;
        }
        a->rpos = 0;
        a->wpos = used;
    }
    return ZRPC_OK;
}

/* ---- 解析并投递完整帧 ---- */

static void tcp_link_dispatch(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link, const uint8_t *body,
                              size_t body_len) {
    zrpc_node_t *node = t->base.node;
    zrpc_envelope_view_t env;

    if (zrpc_envelope_decode(body, body_len, &env) != ZRPC_OK) {
        return;
    }
    {
        zrpc_payload_t pl;
        char route[ZRPC_MAX_ROUTE];
        uint16_t rl = env.route_len;
        if (rl >= ZRPC_MAX_ROUTE) {
            rl = ZRPC_MAX_ROUTE - 1;
        }
        if (rl > 0 && env.route) {
            memcpy(route, env.route, rl);
        } else {
            rl = 0;
        }
        route[rl] = '\0';
        pl.data = env.payload;
        pl.len = env.payload_len;
        pl.encoding = env.encoding;
        {
            zrpc_stream_meta_t sm;
            const zrpc_stream_meta_t *smp = NULL;
            if (env.stream_flags & ZRPC_STREAM_FLAG_STREAM) {
                sm.flags = env.stream_flags &
                           (ZRPC_STREAM_FIRST | ZRPC_STREAM_LAST | ZRPC_STREAM_CANCEL);
                sm.stream_id = env.stream_id;
                sm.offset = env.stream_offset;
                sm.total_size = env.stream_total_size;
                smp = &sm;
            }
            zrpc_node_on_message(node, link->ip, link->port, link, &t->base, env.kind,
                                 env.request_id, route, &pl, smp);
        }
    }
}

static void tcp_link_consume(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link) {
    zrpc_node_t *node = t->base.node;
    size_t max_frame = (size_t)node->max_msg_bytes + ZRPC_MAX_ROUTE + 64;
    zrpc_rx_acc_t *a = &link->rx;

    for (;;) {
        size_t avail = a->wpos - a->rpos;
        const uint8_t *body = NULL;
        size_t body_len = 0;
        size_t frame;

        if (avail < ZRPC_TCP_HDR_LEN) {
            return;
        }
        frame = zrpc_tcp_frame_size(a->buf + a->rpos, avail);
        if (frame == 0 || frame > max_frame) {
            a->rpos = a->wpos = 0; /* 协议错误：丢弃累积 */
            return;
        }
        if (avail < frame) {
            return;
        }
        if (zrpc_tcp_parse(a->buf + a->rpos, frame, &body, &body_len) == 1) {
            node->metrics.tcp_frames_received++;
            tcp_link_dispatch(t, link, body, body_len);
        }
        a->rpos += frame;
        if (a->rpos == a->wpos) {
            a->rpos = a->wpos = 0;
        }
    }
}

static void tcp_link_on_bytes(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link, const void *data,
                              size_t len) {
    if (!link || !data || len == 0) {
        return;
    }
    if (rx_acc_reserve(&link->rx, len) != ZRPC_OK) {
        return;
    }
    memcpy(link->rx.buf + link->rx.wpos, data, len);
    link->rx.wpos += len;
    tcp_link_consume(t, link);
}

/* ---- 出站发送 ---- */

static void tcp_link_flush_tx(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link) {
    (void)t;
    if (link->inbound || !link->connected || !link->client) {
        return;
    }
    while (!zrpc_iobuf_empty(&link->tx)) {
        ztk_socket_iov iov[ZTK_SOCKET_IOV_MAX];
        unsigned n = zrpc_iobuf_iovec(&link->tx, iov, ZTK_SOCKET_IOV_MAX, 0);
        ztk_socket *sock;
        ztk_ssize_t sent;

        if (n == 0) {
            break;
        }
        sock = ztk_tcp_client_socket(link->client);
        if (!sock) {
            break;
        }
        sent = ztk_socket_sendv(sock, iov, n);
        if (sent > 0) {
            zrpc_iobuf_consume(&link->tx, (size_t)sent);
            continue;
        }
        break; /* EAGAIN / 错误：保留队列，交由 tick 或可写事件续写 */
    }
}

/*
 * 发送一帧。快路径直接 sendv(header, payload)，零用户态拷贝；
 * 半写/积压时只把未写后缀拷进待写队列（一次拷贝），随后尽力冲刷。
 */
static int tcp_link_emit(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link, const uint8_t *header,
                         size_t header_len, const void *payload, size_t payload_len) {
    ztk_poller *poller = ztk_poller_current();
    size_t total = header_len + payload_len;

    if (!t || !link || !link->used || !header || header_len == 0) {
        return ZRPC_ERR_INVALID;
    }

    /* 入站会话自带出站队列（ztk_buf 引用），直接委托，避免二次排队。 */
    if (link->inbound) {
        const void *parts[2];
        size_t lens[2];
        ztk_err_t rc;
        if (!link->session) {
            return ZRPC_ERR_STATE;
        }
        parts[0] = header;
        lens[0] = header_len;
        parts[1] = payload;
        lens[1] = payload_len;
        rc = ztk_tcp_session_sendv(link->session, parts, lens, payload_len ? 2u : 1u);
        if (rc == ZTK_OK) {
            t->base.node->metrics.tcp_frames_sent++;
            return ZRPC_OK;
        }
        return (rc == ZTK_ERR_AGAIN) ? ZRPC_ERR_AGAIN : ZRPC_ERR_IO;
    }

    if (!link->client) {
        return ZRPC_ERR_AGAIN;
    }
    if (!link->connected) {
        /* 异步 connect 尚未完成：保留整帧，on_connect 后由 flush_tx 发出。 */
        if (zrpc_iobuf_append_copy(&link->tx, poller, header, header_len) != 0) {
            return ZRPC_ERR_NOMEM;
        }
        if (payload_len && zrpc_iobuf_append_copy(&link->tx, poller, payload, payload_len) != 0) {
            return ZRPC_ERR_NOMEM;
        }
        return ZRPC_OK;
    }

    /* 已有积压：整帧入队（拷贝），冲刷由队列驱动。 */
    if (!zrpc_iobuf_empty(&link->tx)) {
        if (zrpc_iobuf_append_copy(&link->tx, poller, header, header_len) != 0) {
            return ZRPC_ERR_NOMEM;
        }
        if (payload_len && zrpc_iobuf_append_copy(&link->tx, poller, payload, payload_len) != 0) {
            return ZRPC_ERR_NOMEM;
        }
        tcp_link_flush_tx(t, link);
        t->base.node->metrics.tcp_frames_sent++;
        return ZRPC_OK;
    }

    /* 快路径：一次 sendv / send 写出 header + payload。 */
    {
        ztk_socket *sock = ztk_tcp_client_socket(link->client);
        ztk_ssize_t sent;
        if (!sock) {
            return ZRPC_ERR_IO;
        }
        if (payload_len) {
            ztk_socket_iov iov[2];
            iov[0].base = header;
            iov[0].len = header_len;
            iov[1].base = payload;
            iov[1].len = payload_len;
            sent = ztk_socket_sendv(sock, iov, 2);
        } else {
            sent = ztk_socket_send(sock, header, header_len);
        }
        if (sent == (ztk_ssize_t)total) {
            t->base.node->metrics.tcp_frames_sent++;
            return ZRPC_OK;
        }
        if (sent == ZTK_ERR_AGAIN) {
            sent = 0;
        } else if (sent < 0) {
            return ZRPC_ERR_IO;
        }
        /* 半写：只拷贝未写后缀（一次拷贝，替代原先 malloc+两次 memcpy）。 */
        if ((size_t)sent < header_len) {
            size_t hrem = header_len - (size_t)sent;
            if (zrpc_iobuf_append_copy(&link->tx, poller, header + sent, hrem) != 0) {
                return ZRPC_ERR_NOMEM;
            }
            if (payload_len &&
                zrpc_iobuf_append_copy(&link->tx, poller, payload, payload_len) != 0) {
                return ZRPC_ERR_NOMEM;
            }
        } else if (payload_len) {
            size_t off = (size_t)sent - header_len;
            if (zrpc_iobuf_append_copy(&link->tx, poller, (const uint8_t *)payload + off,
                                       payload_len - off) != 0) {
                return ZRPC_ERR_NOMEM;
            }
        }
        tcp_link_flush_tx(t, link);
        t->base.node->metrics.tcp_frames_sent++;
        return ZRPC_OK;
    }
}

/* ---- 连接查找（各自 poller 独占一行，无锁） ---- */

static int io_of_poller(zrpc_tcp_transport_t *t, ztk_poller *poller) {
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

static zrpc_tcp_link_t *link_find_out(zrpc_tcp_transport_t *t, const char *ip, uint16_t port,
                                      int create) {
    zrpc_tcp_link_t *row = t->links[0]; /* 出站固定 io0 */
    int i;
    int free_slot = -1;
    for (i = 0; i < ZRPC_TCP_LINKS_PER_IO; i++) {
        zrpc_tcp_link_t *link = &row[i];
        if (link->used && !link->inbound && link->port == port && strcmp(link->ip, ip) == 0) {
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
        zrpc_tcp_link_t *link = &row[free_slot];
        ztk_tcp_client_ops_t ops;
        ztk_tcp_client_opts_t copts;
        memset(link, 0, sizeof(*link));
        link->used = 1;
        link->inbound = 0;
        rx_acc_init(&link->rx);
        zrpc_iobuf_init(&link->tx);
        zrpc_copy_str(link->ip, sizeof(link->ip), ip, "0.0.0.0");
        link->port = port;

        memset(&ops, 0, sizeof(ops));
        ops.on_connect = tcp_client_on_connect;
        ops.on_recv = tcp_client_on_recv;
        ops.on_error = tcp_client_on_error;
        memset(&copts, 0, sizeof(copts));
        copts.poller = t->base.node->poller;
        copts.ops = &ops;
        copts.user = t;
        link->client = ztk_tcp_client_create(&copts);
        if (!link->client) {
            link->used = 0;
            return NULL;
        }
        if (ztk_tcp_client_connect(link->client, link->ip, link->port) != ZTK_OK) {
            ztk_tcp_client_destroy(link->client);
            link->client = NULL;
            link->used = 0;
            return NULL;
        }
        return link;
    }
}

static zrpc_tcp_link_t *link_for_session(zrpc_tcp_transport_t *t, ztk_tcp_session *session,
                                         int create) {
    int io = io_of_poller(t, ztk_tcp_session_poller(session));
    zrpc_tcp_link_t *row;
    int i;
    int free_slot = -1;
    if (io < 0) {
        io = 0;
    }
    row = t->links[io];
    for (i = 0; i < ZRPC_TCP_LINKS_PER_IO; i++) {
        zrpc_tcp_link_t *link = &row[i];
        if (link->used && link->inbound && link->session == session) {
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
        zrpc_tcp_link_t *link = &row[free_slot];
        ztk_socket *sock;
        char ip[ZRPC_ENDPOINT_MAX];
        uint16_t port = 0;
        memset(link, 0, sizeof(*link));
        link->used = 1;
        link->inbound = 1;
        link->connected = 1;
        link->session = session;
        rx_acc_init(&link->rx);
        zrpc_iobuf_init(&link->tx);
        sock = ztk_tcp_session_socket(session);
        if (sock && ztk_socket_get_peer(sock, ip, sizeof(ip), &port) == ZTK_OK) {
            zrpc_copy_str(link->ip, sizeof(link->ip), ip, "0.0.0.0");
            link->port = port;
        } else {
            zrpc_copy_str(link->ip, sizeof(link->ip), "0.0.0.0", "0.0.0.0");
        }
        return link;
    }
}

static zrpc_tcp_link_t *link_for_client(zrpc_tcp_transport_t *t, ztk_tcp_client *client) {
    zrpc_tcp_link_t *row = t->links[0];
    int i;
    for (i = 0; i < ZRPC_TCP_LINKS_PER_IO; i++) {
        if (row[i].used && !row[i].inbound && row[i].client == client) {
            return &row[i];
        }
    }
    return NULL;
}

/* ---- 服务端会话回调 ---- */

static void tcp_srv_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)user;
    zrpc_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = link_for_session(t, session, 1);
    if (link) {
        tcp_link_on_bytes(t, link, data, len);
    }
}

static void tcp_srv_on_error(ztk_tcp_session *session, void *user) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)user;
    zrpc_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = link_for_session(t, session, 0);
    if (!link) {
        return;
    }
    rx_acc_free(&link->rx);
    zrpc_iobuf_reset(&link->tx);
    link->used = 0;
}

/* ---- 客户端回调 ---- */

static void tcp_client_on_connect(ztk_tcp_client *client, void *user) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)user;
    zrpc_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = link_for_client(t, client);
    if (link) {
        link->connected = 1;
        tcp_link_flush_tx(t, link);
    }
}

static void tcp_client_on_recv(ztk_tcp_client *client, const void *data, size_t len, void *user) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)user;
    zrpc_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = link_for_client(t, client);
    if (link) {
        tcp_link_on_bytes(t, link, data, len);
    }
}

static void tcp_client_on_error(ztk_tcp_client *client, void *user) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)user;
    zrpc_tcp_link_t *link;
    if (!t) {
        return;
    }
    link = link_for_client(t, client);
    if (!link) {
        return;
    }
    rx_acc_free(&link->rx);
    zrpc_iobuf_reset(&link->tx);
    ztk_tcp_client_destroy(client);
    link->client = NULL;
    link->used = 0;
}

/* ---- 传输接口 ---- */

static int tcp_start(zrpc_transport_t *base) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)base;
    zrpc_node_t *node = base->node;
    ztk_tcp_server_opts_t opts;
    ztk_tcp_session_ops_t sops;
    unsigned n;
    unsigned i;

    n = ztk_poller_pool_size(node->pool);
    if (n > ZRPC_IO_MAX) {
        n = ZRPC_IO_MAX;
    }
    t->io_count = (int)n;
    for (i = 0; i < n; i++) {
        t->pollers[i] = ztk_poller_pool_at(node->pool, i);
    }

    memset(&sops, 0, sizeof(sops));
    sops.on_recv = tcp_srv_on_recv;
    sops.on_error = tcp_srv_on_error;
    memset(&opts, 0, sizeof(opts));
    opts.host = node->bind_host;
    opts.port = base->port;
    opts.poller_pool = node->pool;
    opts.session_ops = &sops;
    opts.session_user = t;
    opts.session_create_user = NULL;
    opts.manager_interval_sec = 0.0f;

    t->server = ztk_tcp_server_create(&opts);
    if (!t->server) {
        return ZRPC_ERR_IO;
    }
    if (ztk_tcp_server_start(t->server) != ZTK_OK) {
        ztk_tcp_server_destroy(t->server);
        t->server = NULL;
        return ZRPC_ERR_IO;
    }
    base->port = ztk_tcp_server_port(t->server);
    t->tick_timer = ztk_timer_start(node->poller, ZRPC_TICK_INTERVAL_MS, 1, tcp_tick_cb, t);
    return ZRPC_OK;
}

static void tcp_free_link(zrpc_tcp_link_t *link) {
    if (!link || !link->used) {
        return;
    }
    rx_acc_free(&link->rx);
    zrpc_iobuf_reset(&link->tx);
    if (!link->inbound && link->client) {
        ztk_tcp_client_destroy(link->client);
        link->client = NULL;
    }
    link->used = 0;
}

static void tcp_stop(zrpc_transport_t *base) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)base;
    int i, j;
    if (t->tick_timer) {
        ztk_timer_stop(t->tick_timer);
        t->tick_timer = NULL;
    }
    /* 先停 listen，再销毁 server（会 detach 并销毁所有入站会话），最后清 link 表。 */
    if (t->server) {
        ztk_tcp_server_stop(t->server);
        ztk_tcp_server_destroy(t->server);
        t->server = NULL;
    }
    for (i = 0; i < ZRPC_IO_MAX; i++) {
        for (j = 0; j < ZRPC_TCP_LINKS_PER_IO; j++) {
            tcp_free_link(&t->links[i][j]);
        }
    }
}

static int tcp_send_common(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link, const char *ip,
                           uint16_t port, uint8_t kind, uint32_t msg_id, const char *route,
                           const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    size_t route_len = route ? strlen(route) : 0;
    size_t payload_len = (payload && payload->len) ? payload->len : 0;
    size_t body_len = zrpc_envelope_size(route_len, payload_len);
    size_t need = ZRPC_TCP_HDR_LEN + body_len;
    zrpc_envelope_t header_envelope;

    if (body_len > (size_t)t->base.node->max_msg_bytes + ZRPC_MAX_ROUTE + ZRPC_ENVELOPE_HDR_LEN ||
        need > (size_t)t->base.node->max_msg_bytes + ZRPC_MAX_ROUTE + 64) {
        return ZRPC_ERR_TOOBIG;
    }
    if (!link) {
        link = link_find_out(t, ip, port, 1);
        if (!link) {
            return ZRPC_ERR_NOMEM;
        }
    }
    if (!zrpc_iobuf_empty(&link->tx) &&
        zrpc_iobuf_len(&link->tx) + need > (size_t)t->base.node->bp_high_water) {
        t->base.node->metrics.backpressure_events++;
        return ZRPC_ERR_AGAIN;
    }

    memset(&header_envelope, 0, sizeof(header_envelope));
    header_envelope.kind = kind;
    header_envelope.encoding = payload ? payload->encoding : 0;
    header_envelope.request_id = msg_id;
    header_envelope.route = route;
    if (stream) {
        header_envelope.stream_flags = (uint8_t)(ZRPC_STREAM_FLAG_STREAM | stream->flags);
        header_envelope.stream_id = stream->stream_id;
        header_envelope.stream_offset = stream->offset;
        header_envelope.stream_total_size = stream->total_size;
    }
    header_envelope.payload = NULL;
    header_envelope.payload_len = 0;

    if (route_len < ZRPC_MAX_ROUTE) {
        uint8_t header[ZRPC_TCP_HDR_LEN + ZRPC_ENVELOPE_HDR_LEN + ZRPC_MAX_ROUTE];
        size_t header_len = ZRPC_TCP_HDR_LEN + ZRPC_ENVELOPE_HDR_LEN + route_len;
        zrpc_put_u32(header, (uint32_t)body_len);
        if (zrpc_envelope_encode(header + ZRPC_TCP_HDR_LEN, sizeof(header) - ZRPC_TCP_HDR_LEN,
                                 &header_envelope) != ZRPC_ENVELOPE_HDR_LEN + route_len) {
            return ZRPC_ERR_INVALID;
        }
        return tcp_link_emit(t, link, header, header_len, payload ? payload->data : NULL,
                             payload_len);
    }
    /* 超长 route（正常不会走到）：用池化 header，避免 body+frame 两次分配。 */
    {
        size_t header_len = ZRPC_TCP_HDR_LEN + ZRPC_ENVELOPE_HDR_LEN + route_len;
        uint8_t *header = (uint8_t *)malloc(header_len);
        int rc;
        if (!header) {
            return ZRPC_ERR_NOMEM;
        }
        zrpc_put_u32(header, (uint32_t)body_len);
        if (zrpc_envelope_encode(header + ZRPC_TCP_HDR_LEN, header_len - ZRPC_TCP_HDR_LEN,
                                 &header_envelope) != ZRPC_ENVELOPE_HDR_LEN + route_len) {
            free(header);
            return ZRPC_ERR_INVALID;
        }
        rc = tcp_link_emit(t, link, header, header_len, payload ? payload->data : NULL,
                           payload_len);
        free(header);
        return rc;
    }
}

static int tcp_send(zrpc_transport_t *base, const char *ip, uint16_t port, uint8_t kind,
                    uint32_t msg_id, const char *route, const zrpc_payload_t *payload,
                    const zrpc_stream_meta_t *stream) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)base;
    if (!ip || !port) {
        return ZRPC_ERR_INVALID;
    }
    return tcp_send_common(t, NULL, ip, port, kind, msg_id, route, payload, stream);
}

static int tcp_reply(zrpc_transport_t *base, zrpc_peer_token_t token, const char *ip, uint16_t port,
                     uint8_t kind, uint32_t msg_id, const char *route,
                     const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)base;
    zrpc_tcp_link_t *link = (zrpc_tcp_link_t *)token;
    if (!link || !link->used) {
        return tcp_send(base, ip, port, kind, msg_id, route, payload, stream);
    }
    return tcp_send_common(t, link, link->ip, link->port, kind, msg_id, route, payload, stream);
}

static void tcp_tick_cb(void *user) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)user;
    zrpc_tcp_link_t *row = t->links[0]; /* 只有 io0 上的出站连接需要冲刷 */
    int i;
    for (i = 0; i < ZRPC_TCP_LINKS_PER_IO; i++) {
        zrpc_tcp_link_t *link = &row[i];
        if (link->used && !link->inbound && link->connected && !zrpc_iobuf_empty(&link->tx)) {
            tcp_link_flush_tx(t, link);
        }
    }
}

static const zrpc_transport_ops_t g_tcp_ops = {
    tcp_start,
    tcp_stop,
    tcp_send,
    tcp_reply,
};

zrpc_transport_t *zrpc_tcp_transport_create(void) {
    zrpc_tcp_transport_t *t = (zrpc_tcp_transport_t *)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->base.ops = &g_tcp_ops;
    t->base.kind = ZRPC_TRANSPORT_TCP;
    zrpc_copy_str(t->base.scheme, sizeof(t->base.scheme), "tcp", "tcp");
    return &t->base;
}
