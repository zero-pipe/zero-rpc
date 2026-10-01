/*
 * UDP 传输驱动：RTP 承载 + 分片/重组 + 乱序窗口 + NACK/RTX + AIMD。
 *
 * 每个 io 分片有独立的 socket 与会话表（钉核，无锁）；发现面不在这里。
 * 上层通过 zrpc_node_on_message 收到完整消息。
 */
#include "zrpc_transport.h"

#include "zrpc_internal.h"
#include "zrpc_envelope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

/* 每个对端一个会话（仅在所属 io 分片上访问，无锁） */
typedef struct zrpc_session {
    int used;
    int io;
    zrpc_node_t *node;
    ztk_socket *sock;
    uint32_t peer_ip;
    char peer_ip_s[ZRPC_ENDPOINT_MAX];
    uint16_t peer_port;
    uint64_t last_use_ms;
    uint32_t ssrc_local;
    uint32_t ssrc_remote;
    int ssrc_remote_known;
    uint16_t send_seq;

    zrpc_rx_window_t rx_window;
    uint64_t last_nack_ms;
    uint64_t first_gap_ms;

    zrpc_tx_ring_t tx_ring;

    uint8_t *txq;
    uint32_t txq_sent;
    uint32_t txq_total;
    uint32_t txq_msgid;
    uint32_t cc_burst;
    uint32_t cc_burst_max;
    uint64_t cc_last_loss_ms;
    uint64_t cc_last_grow_ms;

    zrpc_reasm_t reasm;

    uint64_t rx_pkts;
    uint64_t tx_pkts;
    uint64_t retx;
    uint64_t lost;
} zrpc_session_t;

typedef struct zrpc_udp_io_ctx {
    struct zrpc_udp_transport *udp;
    int io;
    uint8_t *rx;
} zrpc_udp_io_ctx_t;

typedef struct zrpc_udp_transport {
    zrpc_transport_t base;
    ztk_socket *socks[ZRPC_IO_MAX];
    ztk_poller *pollers[ZRPC_IO_MAX];
    ztk_timer *tick_timers[ZRPC_IO_MAX];
    zrpc_udp_io_ctx_t io_ctx[ZRPC_IO_MAX];
    zrpc_session_t sessions[ZRPC_IO_MAX][ZRPC_PEERS_PER_IO];
    int io_count;
} zrpc_udp_transport_t;

/* 丢包注入用的确定性伪随机 */
static uint32_t sess_rand(void) {
    static uint32_t st = 0x9e3779b9u;
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static uint32_t udp_rand(void) {
    static uint32_t st;
    if (st == 0) {
        st = (uint32_t)ztk_monotonic_ms() ^ 0x9e3779b9u;
    }
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void session_cleanup(zrpc_session_t *s) {
    if (!s) {
        return;
    }
    zrpc_tx_ring_cleanup(&s->tx_ring);
    if (s->txq) {
        free(s->txq);
        s->txq = NULL;
        s->txq_sent = 0;
    }
    zrpc_reasm_reset(&s->reasm);
    zrpc_rx_window_reset(&s->rx_window);
}

static void session_init(zrpc_udp_transport_t *udp, zrpc_session_t *s, int io, ztk_socket *sock,
                         const char *ip_s, uint16_t port, uint64_t now_ms) {
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->io = io;
    s->node = udp->base.node;
    s->sock = sock;
    zrpc_rx_window_reset(&s->rx_window);
    zrpc_reasm_init(&s->reasm);
    s->cc_burst_max = 1024;
    s->cc_burst = 16;
    s->peer_port = port;
    zrpc_copy_str(s->peer_ip_s, sizeof(s->peer_ip_s), ip_s, "0.0.0.0");
    s->ssrc_local = udp_rand();
    if (s->ssrc_local == 0) {
        s->ssrc_local = 1;
    }
    s->last_use_ms = now_ms;
}

/*
 * 查找/创建某 io 分片上的对端会话。表满时淘汰“最久未使用”的会话，
 * 而不是直接丢包——否则并发对端数超过 ZRPC_PEERS_PER_IO 时，新对端的首请求
 * 会被静默丢弃且永远建不起会话（表现为首请求超时）。
 */
static zrpc_session_t *session_for(zrpc_udp_transport_t *udp, int io, ztk_socket *sock,
                                   const char *ip_s, uint16_t port, uint64_t now_ms) {
    int i;
    int free_slot = -1;
    int lru_slot = -1;
    uint64_t lru_ms = 0;

    if (io < 0 || io >= ZRPC_IO_MAX) {
        return NULL;
    }
    for (i = 0; i < ZRPC_PEERS_PER_IO; i++) {
        zrpc_session_t *s = &udp->sessions[io][i];
        if (s->used) {
            if (s->peer_port == port && strcmp(s->peer_ip_s, ip_s) == 0) {
                s->last_use_ms = now_ms;
                return s;
            }
            if (lru_slot < 0 || s->last_use_ms < lru_ms) {
                lru_slot = i;
                lru_ms = s->last_use_ms;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0 && lru_slot < 0) {
        return NULL;
    }
    {
        int slot = free_slot >= 0 ? free_slot : lru_slot;
        zrpc_session_t *s = &udp->sessions[io][slot];
        if (free_slot < 0) {
            session_cleanup(s); /* 释放被淘汰会话的动态缓冲 */
        }
        session_init(udp, s, io, sock, ip_s, port, now_ms);
        return s;
    }
}

/* ---- 发送 ---- */

static void emit_frag(zrpc_udp_transport_t *udp, zrpc_session_t *s, uint32_t msg_id,
                      uint32_t total_len, uint32_t off, uint32_t chunk, const uint8_t *data,
                      int last, uint64_t now, ztk_udp_dgram_t *msgs, unsigned *nm) {
    zrpc_frag_desc_t d;
    uint8_t *dst;
    size_t n;
    zrpc_node_t *node = udp->base.node;
    uint32_t stride = ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN + node->frag_bytes;

    if (!zrpc_tx_ring_ensure(&s->tx_ring, stride)) {
        return;
    }
    dst = zrpc_tx_ring_slot(&s->tx_ring, s->send_seq);

    memset(&d, 0, sizeof(d));
    d.marker = last ? 1 : 0;
    d.pt = ZRPC_PT_DATA;
    d.seq = s->send_seq;
    d.ts = (uint32_t)now;
    d.ssrc = s->ssrc_local;
    d.msg_id = msg_id;
    d.total_len = total_len;
    d.frag_off = off;
    d.frag_len = chunk;
    d.flags = (uint8_t)((off == 0 ? ZRPC_FLAG_FIRST : 0) | (last ? ZRPC_FLAG_LAST : 0));
    d.chunk = data;

    n = zrpc_wire_build_data(dst, stride, &d);
    if (n == 0) {
        return;
    }
    zrpc_tx_ring_commit(&s->tx_ring, s->send_seq, (uint32_t)n);
    /* 测试用丢包注入：仍填入重传环（NACK 可重传），但不进本批发送 */
    if (!(node->drop_percent && (sess_rand() % 100u) < node->drop_percent)) {
        msgs[*nm].data = dst;
        msgs[*nm].len = n;
        msgs[*nm].ip = s->peer_ip_s;
        msgs[*nm].port = s->peer_port;
        (*nm)++;
    }
    s->send_seq++;
    s->tx_pkts++;
    node->metrics.udp_packets_sent++;
}

static int session_send(zrpc_udp_transport_t *udp, zrpc_session_t *s, uint8_t kind, uint32_t msg_id,
                        const char *route, const zrpc_payload_t *payload,
                        const zrpc_stream_meta_t *stream) {
    ztk_udp_dgram_t msgs[ZRPC_TX_BATCH_MAX];
    unsigned nm = 0;
    size_t route_len = route ? strlen(route) : 0;
    const uint8_t *data = payload ? (const uint8_t *)payload->data : NULL;
    size_t len = payload ? payload->len : 0;
    zrpc_node_t *node = udp->base.node;
    zrpc_envelope_t envelope;
    uint8_t *envbuf;
    size_t env_len;
    uint32_t nfrag;
    size_t off = 0;
    uint32_t i;
    uint64_t now = zrpc_now_ms();

    if (!s || !s->used) {
        return ZRPC_ERR_INVALID;
    }
    if (len > node->max_msg_bytes || route_len >= ZRPC_MAX_ROUTE) {
        return ZRPC_ERR_TOOBIG;
    }
    if (len > 0 && !data) {
        return ZRPC_ERR_INVALID;
    }

    if (s->txq) {
        /* 上一条消息仍在拥塞排空：返回背压，不覆盖，避免隐性丢包。 */
        node->metrics.backpressure_events++;
        return ZRPC_ERR_AGAIN;
    }
    env_len = zrpc_envelope_size(route_len, len);
    if (env_len > (size_t)node->max_msg_bytes + ZRPC_MAX_ROUTE + ZRPC_ENVELOPE_HDR_LEN) {
        return ZRPC_ERR_TOOBIG;
    }
    envbuf = (uint8_t *)malloc(env_len);
    if (!envbuf) {
        return ZRPC_ERR_NOMEM;
    }
    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = kind;
    envelope.encoding = payload ? payload->encoding : 0;
    envelope.request_id = msg_id;
    envelope.route = route;
    envelope.payload = data;
    envelope.payload_len = len;
    if (stream) {
        envelope.stream_flags = (uint8_t)(ZRPC_STREAM_FLAG_STREAM | stream->flags);
        envelope.stream_id = stream->stream_id;
        envelope.stream_offset = stream->offset;
        envelope.stream_total_size = stream->total_size;
    }
    if (zrpc_envelope_encode(envbuf, env_len, &envelope) != env_len) {
        free(envbuf);
        return ZRPC_ERR_INVALID;
    }

    nfrag = env_len ? (uint32_t)((env_len + node->frag_bytes - 1) / node->frag_bytes) : 1;
    for (i = 0; i < nfrag; i++) {
        size_t chunk = env_len - off;
        int last;

        if (chunk > node->frag_bytes) {
            chunk = node->frag_bytes;
        }
        last = (off + chunk) >= env_len;

        if (i < s->cc_burst) {
            if (nm == ZRPC_TX_BATCH_MAX) {
                (void)ztk_socket_sendto_batch(s->sock, msgs, nm);
                nm = 0;
            }
            emit_frag(udp, s, msg_id, (uint32_t)env_len, (uint32_t)off, (uint32_t)chunk,
                      envbuf + off, last, now, msgs, &nm);
        } else {
            s->txq = (uint8_t *)malloc(env_len);
            if (!s->txq) {
                break;
            }
            memcpy(s->txq, envbuf, env_len);
            s->txq_sent = (uint32_t)off;
            s->txq_total = (uint32_t)env_len;
            s->txq_msgid = msg_id;
            break;
        }
        off += chunk;
    }
    if (nm > 0) {
        (void)ztk_socket_sendto_batch(s->sock, msgs, nm);
    }
    free(envbuf);
    return ZRPC_OK;
}

static void session_txq_drain(zrpc_udp_transport_t *udp, zrpc_session_t *s, uint64_t now) {
    ztk_udp_dgram_t msgs[ZRPC_TX_BATCH_MAX];
    unsigned nm = 0;
    uint32_t budget = s->cc_burst;
    zrpc_node_t *node = udp->base.node;

    if (!s->txq) {
        return;
    }
    while (s->txq_sent < s->txq_total && budget-- > 0) {
        uint32_t off = s->txq_sent;
        uint32_t chunk = s->txq_total - s->txq_sent;
        int last;
        if (chunk > node->frag_bytes) {
            chunk = node->frag_bytes;
        }
        last = (off + chunk) >= s->txq_total;
        if (nm == ZRPC_TX_BATCH_MAX) {
            (void)ztk_socket_sendto_batch(s->sock, msgs, nm);
            nm = 0;
        }
        emit_frag(udp, s, s->txq_msgid, s->txq_total, off, chunk, s->txq + off, last, now, msgs,
                  &nm);
        s->txq_sent += chunk;
    }
    if (nm > 0) {
        (void)ztk_socket_sendto_batch(s->sock, msgs, nm);
    }
    if (s->txq_sent >= s->txq_total) {
        free(s->txq);
        s->txq = NULL;
        s->txq_sent = 0;
        s->txq_total = 0;
    }
}

/* ---- 接收 ---- */

static int session_on_data(zrpc_udp_transport_t *udp, zrpc_session_t *s, const zrpc_frag_view_t *v,
                           uint64_t now_ms) {
    zrpc_reasm_slot_t *r;
    uint32_t frag_count;
    uint32_t fidx;
    zrpc_node_t *node = udp->base.node;

    if (!s || !v) {
        return ZRPC_ERR_INVALID;
    }
    s->rx_pkts++;
    node->metrics.udp_packets_received++;
    if (!s->ssrc_remote_known) {
        s->ssrc_remote = v->ssrc;
        s->ssrc_remote_known = 1;
    }

    if (!zrpc_rx_window_accept(&s->rx_window, v->seq)) {
        return ZRPC_OK;
    }

    if (v->total_len > node->max_msg_bytes + ZRPC_MAX_ROUTE + ZRPC_ENVELOPE_HDR_LEN) {
        return ZRPC_ERR_TOOBIG;
    }
    if ((v->frag_off % node->frag_bytes) != 0) {
        return ZRPC_ERR_INVALID;
    }
    frag_count = v->total_len ? (v->total_len + node->frag_bytes - 1) / node->frag_bytes : 1;
    fidx = v->frag_off / node->frag_bytes;
    if (fidx >= frag_count || (uint32_t)v->frag_off + v->frag_len > v->total_len) {
        return ZRPC_ERR_INVALID;
    }

    /* 单分片快路径：解码完整 envelope 后投递。 */
    if (frag_count == 1) {
        zrpc_envelope_view_t env;
        if (zrpc_envelope_decode(v->chunk, v->frag_len, &env) == ZRPC_OK) {
            char route[ZRPC_MAX_ROUTE];
            uint16_t rl = env.route_len;
            zrpc_payload_t pl;
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
                zrpc_node_on_message(node, s->peer_ip_s, s->peer_port, s, &udp->base, env.kind,
                                     env.request_id, route, &pl, smp);
            }
        }
        return ZRPC_OK;
    }

    r = zrpc_reasm_acquire(&s->reasm, v->msg_id, v->total_len, frag_count, now_ms);
    if (!r) {
        return ZRPC_ERR_NOMEM;
    }
    if (!r->base_seq_set) {
        r->base_seq = (uint16_t)(v->seq - fidx);
        r->base_seq_set = 1;
    }
    (void)zrpc_reasm_put(r, fidx, v->frag_off, v->chunk, v->frag_len);

    if (r->got == r->frag_count) {
        zrpc_envelope_view_t env;
        if (zrpc_envelope_decode(r->buf, r->total_len, &env) == ZRPC_OK) {
            char route[ZRPC_MAX_ROUTE];
            uint16_t rl = env.route_len;
            zrpc_payload_t pl;
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
                zrpc_node_on_message(node, s->peer_ip_s, s->peer_port, s, &udp->base, env.kind,
                                     env.request_id, route, &pl, smp);
            }
        }
        zrpc_reasm_release(r);
    }
    return ZRPC_OK;
}

static void session_on_nack(zrpc_udp_transport_t *udp, zrpc_session_t *s, const uint16_t *seqs,
                            size_t n) {
    size_t i;
    if (!s || !seqs) {
        return;
    }
    {
        uint64_t now = zrpc_now_ms();
        if (now - s->cc_last_loss_ms > 20) {
            if (s->cc_burst > 1) {
                s->cc_burst = s->cc_burst / 2;
            }
            s->cc_last_loss_ms = now;
        }
    }
    (void)udp;
    for (i = 0; i < n; i++) {
        uint32_t len = 0;
        const uint8_t *packet = zrpc_tx_ring_get(&s->tx_ring, seqs[i], &len);
        if (packet) {
            (void)ztk_socket_sendto(s->sock, packet, len, s->peer_ip_s, s->peer_port);
            s->retx++;
            if (s->node) {
                s->node->metrics.udp_retransmissions++;
            }
        }
    }
}

static void reasm_on_lost(const zrpc_reasm_slot_t *slot, void *user) {
    zrpc_session_t *s = (zrpc_session_t *)user;
    (void)slot;
    if (s) {
        s->lost++;
        if (s->node) {
            s->node->metrics.udp_reassembly_expired++;
        }
    }
}

static void session_tick(zrpc_udp_transport_t *udp, zrpc_session_t *s, uint64_t now_ms) {
    uint16_t missing[ZRPC_MAX_NACK_SEQS];
    size_t mn = 0;

    if (!s || !s->used) {
        return;
    }

    session_txq_drain(udp, s, now_ms);
    if (s->cc_burst < s->cc_burst_max && now_ms - s->cc_last_loss_ms > 200 &&
        now_ms - s->cc_last_grow_ms > 50) {
        s->cc_burst++;
        s->cc_last_grow_ms = now_ms;
    }

    mn += zrpc_rx_window_missing(&s->rx_window, missing + mn, ZRPC_MAX_NACK_SEQS - mn);
    mn += zrpc_reasm_missing(&s->reasm, missing + mn, ZRPC_MAX_NACK_SEQS - mn);

    if (mn > 0) {
        if (s->first_gap_ms == 0) {
            s->first_gap_ms = now_ms;
        }
        if (now_ms - s->last_nack_ms >= ZRPC_NACK_INTERVAL_MS) {
            uint8_t nack[256];
            size_t n = zrpc_wire_build_nack(nack, sizeof(nack), s->ssrc_local, s->ssrc_remote,
                                            missing, mn);
            if (n > 0) {
                (void)ztk_socket_sendto(s->sock, nack, n, s->peer_ip_s, s->peer_port);
                s->last_nack_ms = now_ms;
                if (s->node) {
                    s->node->metrics.udp_nacks_sent++;
                }
            }
        }
    } else {
        s->first_gap_ms = 0;
    }

    zrpc_reasm_expire(&s->reasm, now_ms, ZRPC_REASM_TIMEOUT_MS, reasm_on_lost, s);
}

/* ---- 数据面接收 ---- */

static void data_recv_cb(ztk_socket *sock, void *user) {
    zrpc_udp_io_ctx_t *ctx = (zrpc_udp_io_ctx_t *)user;
    zrpc_udp_transport_t *udp = ctx ? ctx->udp : NULL;
    int io = ctx ? ctx->io : 0;
    ztk_udp_recv_t recvs[ZRPC_RX_BATCH_MAX];

    if (!udp || !ctx->rx) {
        return;
    }
    for (;;) {
        unsigned i;
        int got;
        for (i = 0; i < ZRPC_RX_BATCH_MAX; i++) {
            recvs[i].data = ctx->rx + (size_t)i * ZRPC_RTX_SLOT_MAX;
            recvs[i].cap = ZRPC_RTX_SLOT_MAX;
            recvs[i].len = 0;
            recvs[i].ip[0] = '\0';
            recvs[i].port = 0;
        }
        got = ztk_socket_recvfrom_batch(sock, recvs, ZRPC_RX_BATCH_MAX);
        if (got <= 0) {
            break;
        }
        {
        uint64_t now = zrpc_now_ms();
        for (i = 0; i < (unsigned)got; i++) {
            uint8_t *buf = (uint8_t *)recvs[i].data;
            size_t n = recvs[i].len;
            if (n < 2) {
                continue;
            }
            if ((buf[1] & 0x7f) == ZRPC_RTCP_RTPFB) {
                uint16_t seqs[ZRPC_MAX_NACK_SEQS];
                int c = zrpc_wire_parse_nack(buf, n, NULL, NULL, seqs, ZRPC_MAX_NACK_SEQS);
                if (c > 0) {
                    zrpc_session_t *s = session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                    if (s) {
                        session_on_nack(udp, s, seqs, (size_t)c);
                    }
                }
                continue;
            }
            {
                zrpc_frag_view_t v;
                int r = zrpc_wire_parse_data(buf, n, &v);
                if (r == 1) {
                    zrpc_session_t *s = session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                    if (s) {
                        (void)session_on_data(udp, s, &v, now);
                    }
                }
            }
        }
        }
    }
}

/* ---- 定时器 ---- */

static void udp_tick_cb(void *user) {
    zrpc_udp_io_ctx_t *ctx = (zrpc_udp_io_ctx_t *)user;
    zrpc_udp_transport_t *udp = ctx->udp;
    uint64_t now = zrpc_now_ms();
    int i;
    for (i = 0; i < ZRPC_PEERS_PER_IO; i++) {
        session_tick(udp, &udp->sessions[ctx->io][i], now);
    }
}

/* ---- 传输接口 ---- */

static int udp_start(zrpc_transport_t *t) {
    zrpc_udp_transport_t *udp = (zrpc_udp_transport_t *)t;
    zrpc_node_t *node = t->node;
    ztk_socket_callbacks_t cbs;
    uint16_t actual = 0;
    int i;

    udp->io_count = 0;
    for (i = 0; i < (int)node->io_threads && i < ZRPC_IO_MAX; i++) {
        ztk_socket *ds = ztk_socket_create();
        if (!ds) {
            break;
        }
        if (ztk_socket_bind_udp_ex(ds, node->bind_host, t->port, 1, i > 0 ? 1 : 0) != ZTK_OK) {
            ztk_socket_destroy(ds);
            break;
        }
        if (i == 0 && ztk_socket_get_local(ds, NULL, 0, &actual) == ZTK_OK) {
            t->port = actual;
        }
#if !defined(_WIN32)
        {
            int fd = ztk_socket_fd(ds);
            int sz = 8 * 1024 * 1024;
            if (fd >= 0) {
                (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, (socklen_t)sizeof(sz));
                (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, (socklen_t)sizeof(sz));
            }
        }
#endif
        udp->io_ctx[i].udp = udp;
        udp->io_ctx[i].io = i;
        udp->io_ctx[i].rx = (uint8_t *)malloc((size_t)ZRPC_RX_BATCH_MAX * ZRPC_RTX_SLOT_MAX);
        if (!udp->io_ctx[i].rx) {
            ztk_socket_destroy(ds);
            break;
        }
        memset(&cbs, 0, sizeof(cbs));
        cbs.on_readable = data_recv_cb;
        udp->pollers[i] = ztk_poller_pool_at(node->pool, (unsigned)i);
        if (ztk_socket_attach_poller(ds, udp->pollers[i], &cbs, &udp->io_ctx[i]) != ZTK_OK) {
            free(udp->io_ctx[i].rx);
            udp->io_ctx[i].rx = NULL;
            ztk_socket_destroy(ds);
            break;
        }
        udp->socks[i] = ds;
        udp->io_count++;
    }
    if (udp->io_count == 0) {
        return ZRPC_ERR_IO;
    }
    for (i = 0; i < udp->io_count; i++) {
        udp->tick_timers[i] = ztk_timer_start(udp->pollers[i], ZRPC_TICK_INTERVAL_MS, 1,
                                              udp_tick_cb, &udp->io_ctx[i]);
    }
    return ZRPC_OK;
}

static void udp_stop(zrpc_transport_t *t) {
    zrpc_udp_transport_t *udp = (zrpc_udp_transport_t *)t;
    int i, j;
    for (i = 0; i < ZRPC_IO_MAX; i++) {
        if (udp->tick_timers[i]) {
            ztk_timer_stop(udp->tick_timers[i]);
            udp->tick_timers[i] = NULL;
        }
    }
    for (i = 0; i < ZRPC_IO_MAX; i++) {
        if (udp->socks[i]) {
            ztk_socket_detach_poller(udp->socks[i]);
            ztk_socket_destroy(udp->socks[i]);
            udp->socks[i] = NULL;
        }
        if (udp->io_ctx[i].rx) {
            free(udp->io_ctx[i].rx);
            udp->io_ctx[i].rx = NULL;
        }
        for (j = 0; j < ZRPC_PEERS_PER_IO; j++) {
            if (udp->sessions[i][j].used) {
                session_cleanup(&udp->sessions[i][j]);
                udp->sessions[i][j].used = 0;
            }
        }
    }
}

static int udp_send(zrpc_transport_t *t, const char *ip, uint16_t port, uint8_t kind,
                    uint32_t msg_id, const char *route, const zrpc_payload_t *payload,
                    const zrpc_stream_meta_t *stream) {
    zrpc_udp_transport_t *udp = (zrpc_udp_transport_t *)t;
    zrpc_session_t *s;
    if (!ip || !port || udp->io_count == 0) {
        return ZRPC_ERR_INVALID;
    }
    s = session_for(udp, 0, udp->socks[0], ip, port, zrpc_now_ms());
    if (!s) {
        return ZRPC_ERR_NOMEM;
    }
    return session_send(udp, s, kind, msg_id, route, payload, stream);
}

static int udp_reply(zrpc_transport_t *t, zrpc_peer_token_t token, const char *ip, uint16_t port,
                     uint8_t kind, uint32_t msg_id, const char *route,
                     const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    zrpc_udp_transport_t *udp = (zrpc_udp_transport_t *)t;
    zrpc_session_t *s = (zrpc_session_t *)token;
    if (!s || !s->used) {
        if (!ip || !port || udp->io_count == 0) {
            return ZRPC_ERR_INVALID;
        }
        s = session_for(udp, 0, udp->socks[0], ip, port, zrpc_now_ms());
        if (!s) {
            return ZRPC_ERR_NOMEM;
        }
    }
    return session_send(udp, s, kind, msg_id, route, payload, stream);
}

static const zrpc_transport_ops_t g_udp_ops = {
    udp_start,
    udp_stop,
    udp_send,
    udp_reply,
};

zrpc_transport_t *zrpc_udp_transport_create(void) {
    zrpc_udp_transport_t *udp = (zrpc_udp_transport_t *)calloc(1, sizeof(*udp));
    if (!udp) {
        return NULL;
    }
    udp->base.ops = &g_udp_ops;
    udp->base.kind = ZRPC_TRANSPORT_UDP;
    zrpc_copy_str(udp->base.scheme, sizeof(udp->base.scheme), "udp", "udp");
    return &udp->base;
}
