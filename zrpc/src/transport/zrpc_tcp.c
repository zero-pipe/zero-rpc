/*
 * TCP 传输驱动：基于 ztk_tcp_server / ztk_tcp_client 的长度前缀分帧。
 *
 * 线程模型：入站会话由 ztk_tcp_server 分散到 poller_pool 的各个 poller，出站连接
 * 固定在 io0。为避免跨线程争用，link 表按 poller 分片（每个 poller 独占一行），
 * 热路径无锁；同一会话的 on_recv/on_error 始终在其所属 poller 上执行。
 *
 * 无分片/重传：TCP 自身可靠有序。
 */
#include "zrpc_transport.h"

#include "zrpc_internal.h"
#include "zrpc_envelope.h"

#include <stdlib.h>
#include <string.h>

/* 一条 TCP 连接：入站（服务端会话）或出站（客户端连接）。 */
typedef struct zrpc_tcp_link {
    int used;
    int inbound;
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port;
    ztk_tcp_session *session; /* inbound */
    ztk_tcp_client *client;   /* outbound */
    int connected;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
    uint8_t *tx; /* connected 前/半写时的待发帧 */
    size_t tx_len;
    size_t tx_cap;
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

/* ---- 缓冲区 ---- */

static int buf_reserve(uint8_t **buf, size_t *cap, size_t need, size_t max) {
    uint8_t *p;
    size_t next;
    if (need <= *cap) {
        return ZRPC_OK;
    }
    next = *cap ? *cap : 4096;
    while (next < need) {
        if (next > max) {
            return ZRPC_ERR_TOOBIG;
        }
        next *= 2;
    }
    p = (uint8_t *)realloc(*buf, next);
    if (!p) {
        return ZRPC_ERR_NOMEM;
    }
    *buf = p;
    *cap = next;
    return ZRPC_OK;
}

/* ---- 解析并投递完整帧 ---- */

static void tcp_link_consume(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link) {
    zrpc_node_t *node = t->base.node;
    while (link->rx_len >= 4) {
        size_t frame = zrpc_tcp_frame_size(link->rx, link->rx_len);
        const uint8_t *body = NULL;
        size_t body_len = 0;
        if (frame == 0 || frame > (size_t)node->max_msg_bytes + ZRPC_MAX_ROUTE + 64) {
            link->rx_len = 0; /* 协议错误：丢弃累积 */
            return;
        }
        if (link->rx_len < frame) {
            return;
        }
        if (zrpc_tcp_parse(link->rx, frame, &body, &body_len) == 1) {
            node->metrics.tcp_frames_received++;
            zrpc_envelope_view_t env;
            if (zrpc_envelope_decode(body, body_len, &env) == ZRPC_OK) {
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
        link->rx_len -= frame;
        if (link->rx_len > 0) {
            memmove(link->rx, link->rx + frame, link->rx_len);
        }
    }
}

static void tcp_link_on_bytes(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link, const void *data,
                              size_t len) {
    if (buf_reserve(&link->rx, &link->rx_cap, link->rx_len + len,
                    (size_t)t->base.node->max_msg_bytes + ZRPC_MAX_ROUTE + 64) != ZRPC_OK) {
        return;
    }
    memcpy(link->rx + link->rx_len, data, len);
    link->rx_len += len;
    tcp_link_consume(t, link);
}

/* ---- 出站发送 ---- */

static int tcp_link_flush(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link) {
    (void)t;
    if (link->tx_len == 0) {
        return ZRPC_OK;
    }
    if (link->inbound) {
        /* 服务端会话自带出站队列，交给它排队/发送；失败即丢弃本帧。 */
        if (link->session) {
            (void)ztk_tcp_session_send(link->session, link->tx, link->tx_len);
        }
        link->tx_len = 0;
        return ZRPC_OK;
    }
    if (!link->connected || !link->client) {
        return ZRPC_OK; /* 待连接成功后冲刷 */
    }
    {
        /* 半写保护：按实际写入字节数推进，避免整段重发导致重复数据。 */
        ztk_ssize_t sent = ztk_tcp_client_send(link->client, link->tx, link->tx_len);
        if (sent < 0) {
            if (sent == ZTK_ERR_AGAIN) {
                return ZRPC_ERR_AGAIN; /* 保留队列，tick 再试 */
            }
            return ZRPC_ERR_IO;
        }
        if ((size_t)sent < link->tx_len) {
            memmove(link->tx, link->tx + (size_t)sent, link->tx_len - (size_t)sent);
            link->tx_len -= (size_t)sent;
            return ZRPC_ERR_AGAIN; /* 仍有剩余，tick 再试 */
        }
    }
    link->tx_len = 0;
    return ZRPC_OK;
}

static int tcp_link_queue_and_flush(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link,
                                    const uint8_t *frame, size_t len) {
    int rc;
    if (link->tx_len + len > (size_t)t->base.node->max_msg_bytes + ZRPC_MAX_ROUTE + 64) {
        return ZRPC_ERR_TOOBIG;
    }
    if (buf_reserve(&link->tx, &link->tx_cap, link->tx_len + len,
                    (size_t)t->base.node->max_msg_bytes + ZRPC_MAX_ROUTE + 64) != ZRPC_OK) {
        return ZRPC_ERR_NOMEM;
    }
    memcpy(link->tx + link->tx_len, frame, len);
    link->tx_len += len;
    rc = tcp_link_flush(t, link);
    return rc == ZRPC_ERR_AGAIN ? ZRPC_OK : rc;
}

/* Fast path: keep the borrowed payload out of an intermediate frame copy. */
static int tcp_sendv_fast(zrpc_tcp_transport_t *t, zrpc_tcp_link_t *link,
                          const uint8_t *header, size_t header_len, const void *payload,
                          size_t payload_len) {
    const void *parts[2];
    size_t lens[2];
    size_t total = header_len + payload_len;

    if (!t || !link || !link->used || link->tx_len != 0 || !header || header_len == 0) {
        return ZRPC_ERR_AGAIN;
    }
    parts[0] = header;
    lens[0] = header_len;
    parts[1] = payload;
    lens[1] = payload_len;

    if (link->inbound) {
        ztk_err_t rc;
        if (!link->session) {
            return ZRPC_ERR_AGAIN;
        }
        rc = ztk_tcp_session_sendv(link->session, parts, lens, payload_len ? 2u : 1u);
        return rc == ZTK_OK ? ZRPC_OK : (rc == ZTK_ERR_AGAIN ? ZRPC_ERR_AGAIN : ZRPC_ERR_IO);
    }

    if (!link->connected || !link->client) {
        return ZRPC_ERR_AGAIN;
    }
    {
        ztk_socket_iov iov[2];
        iov[0].base = header;
        iov[0].len = header_len;
        iov[1].base = payload;
        iov[1].len = payload_len;
        ztk_ssize_t sent = ztk_socket_sendv(ztk_tcp_client_socket(link->client), iov,
                                            payload_len ? 2u : 1u);
        if (sent == (ztk_ssize_t)total) {
            return ZRPC_OK;
        }
        if (sent == ZTK_ERR_AGAIN) {
            sent = 0;
        } else if (sent < 0) {
            return ZRPC_ERR_IO;
        }
        if ((size_t)sent < total) {
            size_t done = (size_t)sent;
            size_t remaining = total - done;
            uint8_t *copy = (uint8_t *)malloc(remaining);
            int rc;
            if (!copy) {
                return ZRPC_ERR_NOMEM;
            }
            if (done < header_len) {
                size_t header_remaining = header_len - done;
                memcpy(copy, header + done, header_remaining);
                if (payload_len > 0) {
                    memcpy(copy + header_remaining, payload, payload_len);
                }
            } else if (payload_len > 0) {
                memcpy(copy, (const uint8_t *)payload + (done - header_len), remaining);
            }
            rc = tcp_link_queue_and_flush(t, link, copy, remaining);
            free(copy);
            return rc;
        }
    }
    return ZRPC_OK;
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
    if (link->rx) {
        free(link->rx);
    }
    if (link->tx) {
        free(link->tx);
    }
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
        (void)tcp_link_flush(t, link);
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
    if (link->rx) {
        free(link->rx);
    }
    if (link->tx) {
        free(link->tx);
    }
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
    if (link->rx) {
        free(link->rx);
        link->rx = NULL;
    }
    if (link->tx) {
        free(link->tx);
        link->tx = NULL;
    }
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
    zrpc_envelope_t envelope;
    uint8_t *body;
    uint8_t *frame;
    size_t route_len = route ? strlen(route) : 0;
    size_t payload_len = (payload && payload->len) ? payload->len : 0;
    size_t body_len;
    size_t need;
    size_t n;
    int rc;

    body_len = zrpc_envelope_size(route_len, payload_len);
    need = ZRPC_TCP_HDR_LEN + body_len;
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
    if (link->tx_len > 0 &&
        link->tx_len + need > (size_t)t->base.node->bp_high_water) {
        t->base.node->metrics.backpressure_events++;
        return ZRPC_ERR_AGAIN;
    }
    if (route_len < ZRPC_MAX_ROUTE) {
        uint8_t header[ZRPC_TCP_HDR_LEN + ZRPC_ENVELOPE_HDR_LEN + ZRPC_MAX_ROUTE];
        size_t header_len = ZRPC_TCP_HDR_LEN + ZRPC_ENVELOPE_HDR_LEN + route_len;
        int fast_rc;
        zrpc_envelope_t header_envelope;
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
        zrpc_put_u32(header, (uint32_t)body_len);
        if (zrpc_envelope_encode(header + ZRPC_TCP_HDR_LEN, sizeof(header) - ZRPC_TCP_HDR_LEN,
                                 &header_envelope) != ZRPC_ENVELOPE_HDR_LEN + route_len) {
            return ZRPC_ERR_INVALID;
        }
        fast_rc = tcp_sendv_fast(t, link, header, header_len, payload ? payload->data : NULL,
                                 payload_len);
        if (fast_rc != ZRPC_ERR_AGAIN) {
            if (fast_rc == ZRPC_OK) {
                t->base.node->metrics.tcp_frames_sent++;
            }
            return fast_rc;
        }
    }
    body = (uint8_t *)malloc(body_len);
    frame = (uint8_t *)malloc(need);
    if (!body || !frame) {
        free(body);
        free(frame);
        return ZRPC_ERR_NOMEM;
    }
    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = kind;
    envelope.encoding = payload ? payload->encoding : 0;
    envelope.request_id = msg_id;
    envelope.route = route;
    envelope.payload = payload ? payload->data : NULL;
    envelope.payload_len = payload_len;
    if (stream) {
        envelope.stream_flags = (uint8_t)(ZRPC_STREAM_FLAG_STREAM | stream->flags);
        envelope.stream_id = stream->stream_id;
        envelope.stream_offset = stream->offset;
        envelope.stream_total_size = stream->total_size;
    }
    if (zrpc_envelope_encode(body, body_len, &envelope) != body_len) {
        free(body);
        free(frame);
        return ZRPC_ERR_INVALID;
    }
    n = zrpc_tcp_build(frame, need, body, body_len);
    rc = n ? tcp_link_queue_and_flush(t, link, frame, n) : ZRPC_ERR_TOOBIG;
    if (rc == ZRPC_OK) {
        t->base.node->metrics.tcp_frames_sent++;
    }
    free(body);
    free(frame);
    return rc;
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
        if (link->used && !link->inbound && link->tx_len > 0 && link->connected) {
            (void)tcp_link_flush(t, link);
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
