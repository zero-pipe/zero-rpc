/*
 * UDP 传输驱动：RTP 承载 + 分片/重组 + 乱序窗口 + NACK/RTX + AIMD。
 *
 * 每个 io 分片有独立的 socket 与会话表（钉核，无锁）；发现面不在这里。
 * 上层通过 zri_node_on_message 收到完整消息。
 */
#include "transport_internal.h"

#include "envelope.h"
#include "reasm.h"
#include "rx_window.h"
#include "tx_ring.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

/* 每个对端一个会话（仅在所属 io 分片上访问，无锁） */
typedef struct zri_session {
    int used;
    int io;
    metrics_t *metrics;
    ntk_socket *sock;
    uint32_t peer_ip;
    char peer_ip_s[ENDPOINT_MAX];
    uint16_t peer_port;
    uint64_t last_use_ms;
    uint32_t ssrc_local;
    uint32_t ssrc_remote;
    int ssrc_remote_known;
    uint16_t send_seq;
    uint64_t last_rx_data_ms; /* recv-side: last time peer data arrived (sender RTO) */
    uint16_t acked_upto;      /* highest send seq inferred received by peer */
    uint8_t rto_attempts;     /* idle-RTO retransmit rounds; reset on peer data */

    zri_rx_window_t rx_window;
    uint64_t last_nack_ms;
    uint64_t first_gap_ms;

    zri_tx_ring_t tx_ring;

    uint8_t *txq;
    uint32_t txq_sent;
    uint32_t txq_total;
    uint32_t txq_msgid;
    uint32_t cc_burst;
    uint32_t cc_burst_max;
    uint64_t cc_last_loss_ms;
    uint64_t cc_last_grow_ms;

    zri_reasm_t reasm;

    uint64_t rx_pkts;
    uint64_t tx_pkts;
    uint64_t retx;
    uint64_t lost;

    /* 整包 XOR 前向纠错（env FEC=1）：缓存最近一个 FEC 包以重建单个丢失分片 */
    uint16_t fec_seq; /* FEC 包独立序号（不占用数据序号空间） */
    uint8_t *fec_rx_buf;
    uint32_t fec_rx_len;
    uint32_t fec_rx_msgid;
    int fec_rx_valid;

    /* RTCP 报告状态（RFC 3550 SR/RR） */
    uint64_t rtcp_last_send_ms;
    uint32_t rtcp_pkt_count;      /* 本端发出的 RTP 包数（供 SR） */
    uint32_t rtcp_octet_count;    /* 本端发出的 RTP payload 字节数（供 SR） */
    uint32_t rtcp_last_rtp_ts;    /* 最近发送的 RTP 时间戳 */
    uint64_t rtcp_last_rtp_wall;  /* 该时间戳对应的墙钟毫秒 */
    uint32_t rtcp_last_sr_ntp;    /* 本端最近发出 SR 的 NTP middle32 */
    int rx_max_seq_set;
    uint16_t rx_max_seq;
    uint32_t rx_cycles;           /* 序号回绕计数 */
    uint32_t rx_received;
    uint32_t rx_expected_prior;   /* 上次报告时的期望包数 */
    uint32_t rx_received_prior;   /* 上次报告时的实收包数 */
    double jitter;                /* 接收抖动估计（毫秒） */
    int have_transit;
    int64_t transit_prev;
    uint32_t last_rx_sr_ntp;      /* 对端最近 SR 的 NTP middle32 */
    uint64_t last_rx_sr_ms;       /* 收到该 SR 的本端单调时刻 */
    int have_last_rx_sr;
    uint32_t last_rtt_ms;         /* 由对端报告块测得 */
    int32_t rtcp_remote_lost;     /* 对端报告的本端流累计丢包 */
    uint64_t rtcp_sr_sent;        /* 本会话 SR/RR 收发明细（供按对端查询） */
    uint64_t rtcp_rr_sent;
    uint64_t rtcp_sr_received;
    uint64_t rtcp_rr_received;
} zri_session_t;

typedef struct zri_udp_io_ctx {
    struct zri_udp_transport *udp;
    int io;
    uint8_t *rx;
} zri_udp_io_ctx_t;

typedef struct zri_udp_transport {
    zri_transport_t base;
    ntk_socket *socks[IO_MAX];
    ntk_poller *pollers[IO_MAX];
    ntk_timer *tick_timers[IO_MAX];
    zri_udp_io_ctx_t io_ctx[IO_MAX];
    zri_session_t sessions[IO_MAX][PEERS_PER_IO];
    int io_count;
} zri_udp_transport_t;

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
        st = (uint32_t)ntk_monotonic_ms() ^ 0x9e3779b9u;
    }
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void session_cleanup(zri_session_t *s) {
    if (!s) {
        return;
    }
    zri_tx_ring_cleanup(&s->tx_ring);
    if (s->txq) {
        free(s->txq);
        s->txq = NULL;
        s->txq_sent = 0;
    }
    if (s->fec_rx_buf) {
        free(s->fec_rx_buf);
        s->fec_rx_buf = NULL;
    }
    zri_reasm_reset(&s->reasm);
    zri_rx_window_reset(&s->rx_window);
}

static void session_send_bye(zri_udp_transport_t *udp, zri_session_t *s);

static void session_init(zri_udp_transport_t *udp, zri_session_t *s, int io, ntk_socket *sock,
                         const char *ip_s, uint16_t port, uint64_t now_ms) {
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->io = io;
    s->metrics = udp->base.metrics;
    s->sock = sock;
    zri_rx_window_reset(&s->rx_window);
    zri_reasm_init(&s->reasm);
    s->cc_burst_max = 1024;
    s->cc_burst = 16;
    s->peer_port = port;
    zri_copy_str(s->peer_ip_s, sizeof(s->peer_ip_s), ip_s, "0.0.0.0");
    s->ssrc_local = udp_rand();
    if (s->ssrc_local == 0) {
        s->ssrc_local = 1;
    }
    s->last_use_ms = now_ms;
    s->last_rx_data_ms = now_ms;
    s->rtcp_last_send_ms = now_ms;
}

/*
 * 查找/创建某 io 分片上的对端会话。表满时淘汰“最久未使用”的会话，
 * 而不是直接丢包——否则并发对端数超过 PEERS_PER_IO 时，新对端的首请求
 * 会被静默丢弃且永远建不起会话（表现为首请求超时）。
 */
static zri_session_t *session_for(zri_udp_transport_t *udp, int io, ntk_socket *sock,
                                   const char *ip_s, uint16_t port, uint64_t now_ms) {
    int i;
    int free_slot = -1;
    int lru_slot = -1;
    uint64_t lru_ms = 0;

    if (io < 0 || io >= IO_MAX) {
        return NULL;
    }
    for (i = 0; i < PEERS_PER_IO; i++) {
        zri_session_t *s = &udp->sessions[io][i];
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
        zri_session_t *s = &udp->sessions[io][slot];
        if (free_slot < 0) {
            session_send_bye(udp, s); /* 淘汰前通知对端 */
            session_cleanup(s); /* 释放被淘汰会话的动态缓冲 */
        }
        session_init(udp, s, io, sock, ip_s, port, now_ms);
        return s;
    }
}

/* ---- 发送 ---- */

/*
 * 逻辑 envelope 视图：[prefix = envelope 头 + route][payload]。
 * prefix 常驻栈/池，payload 借用调用方内存，避免整包 envbuf 的分配与拷贝。
 */
typedef struct zri_env_view {
    const uint8_t *prefix;
    size_t prefix_len;
    const uint8_t *payload;
    size_t payload_len;
} zri_env_view_t;

/* 从逻辑视图的 off 处拷贝 len 字节到 dst。 */
static void env_view_copy(const zri_env_view_t *v, size_t off, size_t len, uint8_t *dst) {
    if (off < v->prefix_len) {
        size_t n = v->prefix_len - off;
        if (n > len) {
            n = len;
        }
        memcpy(dst, v->prefix + off, n);
        dst += n;
        len -= n;
        off = 0; /* 剩余落在 payload 起始处 */
    } else {
        off -= v->prefix_len;
    }
    if (len > 0) {
        memcpy(dst, v->payload + off, len);
    }
}

static void emit_frag(zri_udp_transport_t *udp, zri_session_t *s, uint32_t msg_id,
                      uint32_t total_len, uint32_t off, uint32_t chunk, const zri_env_view_t *view,
                      int last, uint64_t now, ntk_udp_dgram_t *msgs, unsigned *nm) {
    zri_frag_t d;
    uint8_t *dst;
    size_t hdr;
    size_t n;
    uint32_t stride = RTP_HDR_LEN + FRAG_HDR_LEN + udp->base.frag_bytes;

    if (!zri_tx_ring_ensure(&s->tx_ring, stride)) {
        return;
    }
    dst = zri_tx_ring_slot(&s->tx_ring, s->send_seq);

    memset(&d, 0, sizeof(d));
    d.marker = last ? 1 : 0;
    d.pt = PT_DATA;
    d.seq = s->send_seq;
    d.ts = (uint32_t)now;
    d.ssrc = s->ssrc_local;
    d.msg_id = msg_id;
    d.total_len = total_len;
    d.frag_off = off;
    d.frag_len = chunk;
    d.flags = (uint8_t)((off == 0 ? FLAG_FIRST : 0) | (last ? FLAG_LAST : 0));
    d.chunk = NULL;

    /* 只写分片头，随后从逻辑视图拷贝分片数据（payload 只拷这一次，用于重传环）。 */
    hdr = zri_frame_build_frag_header(dst, stride, &d);
    if (hdr == 0) {
        return;
    }
    env_view_copy(view, off, chunk, dst + hdr);
    n = hdr + chunk;
    zri_tx_ring_commit(&s->tx_ring, s->send_seq, (uint32_t)n);
    zri_tx_ring_mark_sent(&s->tx_ring, s->send_seq, now);
    /* 测试用丢包注入：仍填入重传环（NACK 可重传），但不进本批发送 */
    if (!(udp->base.drop_percent && (sess_rand() % 100u) < udp->base.drop_percent)) {
        msgs[*nm].data = dst;
        msgs[*nm].len = n;
        msgs[*nm].ip = s->peer_ip_s;
        msgs[*nm].port = s->peer_port;
        (*nm)++;
    }
    s->send_seq++;
    s->tx_pkts++;
    udp->base.metrics->udp_packets_sent++;
    /* RTCP Sender Report 的收发计数与时间戳映射 */
    s->rtcp_pkt_count++;
    s->rtcp_octet_count += chunk;
    s->rtcp_last_rtp_ts = d.ts;
    s->rtcp_last_rtp_wall = ntk_wall_ms();
}

/* ---- 整包 XOR 前向纠错（env FEC=1，默认关） ----
 * 发送端为每个多分片消息额外发 1 个 XOR 校验包；接收端若该消息恰好缺 1 个分片，
 * 可用校验包 XOR 其余已收分片重建它，省掉一次 NACK/RTX 往返。 */

static int session_on_data(zri_udp_transport_t *udp, zri_session_t *s, const zri_frag_t *v,
                           uint64_t now_ms);

static int udp_fec_enabled(void) {
    static int inited = 0;
    static int on = 0;
    if (!inited) {
        const char *e = getenv("FEC");
        on = (e && e[0] == '1') ? 1 : 0;
        inited = 1;
    }
    return on;
}

static void send_fec(zri_udp_transport_t *udp, zri_session_t *s, const zri_env_view_t *view,
                     size_t env_len, uint32_t msg_id) {
    uint8_t parity[FRAG_MAX];
    uint8_t pkt[RTP_HDR_LEN + FRAG_HDR_LEN + FRAG_MAX];
    size_t frag = udp->base.frag_bytes;
    size_t parity_len = env_len < frag ? env_len : frag;
    size_t p, hdr;
    zri_frag_t d;

    if (frag == 0 || parity_len == 0) {
        return;
    }
    memset(parity, 0, parity_len);
    for (p = 0; p < env_len; p++) {
        uint8_t b = (p < view->prefix_len) ? view->prefix[p] : view->payload[p - view->prefix_len];
        parity[p % frag] ^= b;
    }
    memset(&d, 0, sizeof(d));
    d.pt = PT_FEC;
    d.seq = s->fec_seq++;
    d.ts = (uint32_t)zri_now_ms();
    d.ssrc = s->ssrc_local;
    d.msg_id = msg_id;
    d.total_len = (uint32_t)env_len;
    d.frag_off = 0;
    d.frag_len = (uint32_t)parity_len;
    hdr = zri_frame_build_frag_header(pkt, sizeof(pkt), &d);
    if (hdr == 0) {
        return;
    }
    memcpy(pkt + hdr, parity, parity_len);
    (void)ntk_socket_sendto(s->sock, pkt, hdr + parity_len, s->peer_ip_s, s->peer_port);
}

static void fec_try_reconstruct(zri_udp_transport_t *udp, zri_session_t *s, uint32_t msg_id,
                                uint64_t now_ms) {
    zri_reasm_slot_t *slot;
    uint32_t frag = udp->base.frag_bytes;
    uint32_t total, frag_count, m = 0, idx, k;
    uint8_t acc[FRAG_MAX];
    int missing = -1;
    zri_frag_t v2;

    if (!udp_fec_enabled() || !s->fec_rx_valid || s->fec_rx_msgid != msg_id || !s->fec_rx_buf ||
        frag == 0) {
        return;
    }
    slot = zri_reasm_find(&s->reasm, msg_id);
    if (!slot || !slot->buf) {
        return;
    }
    total = slot->total_len;
    frag_count = (total + frag - 1) / frag;
    if (frag_count != slot->frag_count || slot->got + 1 != frag_count) {
        return; /* 只处理恰好缺 1 片 */
    }
    for (idx = 0; idx < frag_count; idx++) {
        if (!(slot->bits[idx >> 3] & (uint8_t)(1u << (idx & 7)))) {
            missing = (int)idx;
            break;
        }
    }
    if (missing < 0) {
        return;
    }
    m = (uint32_t)missing;
    memcpy(acc, s->fec_rx_buf, s->fec_rx_len);
    for (idx = 0; idx < frag_count; idx++) {
        uint32_t off, len;
        if (idx == m || !(slot->bits[idx >> 3] & (uint8_t)(1u << (idx & 7)))) {
            continue;
        }
        off = idx * frag;
        len = (total - off) < frag ? (total - off) : frag;
        if (len > s->fec_rx_len) {
            len = s->fec_rx_len;
        }
        for (k = 0; k < len; k++) {
            acc[k] ^= slot->buf[off + k];
        }
    }
    {
        uint32_t off = m * frag;
        uint32_t len = (total - off) < frag ? (total - off) : frag;
        memset(&v2, 0, sizeof(v2));
        v2.marker = (m + 1 == frag_count) ? 1 : 0;
        v2.pt = PT_DATA;
        v2.seq = (uint16_t)(slot->base_seq + m);
        v2.ssrc = s->ssrc_remote;
        v2.msg_id = msg_id;
        v2.total_len = total;
        v2.frag_off = off;
        v2.frag_len = len;
        v2.chunk = acc;
        s->fec_rx_valid = 0; /* 已消费 */
        if (s->metrics) {
            s->metrics->udp_retransmissions++; /* FEC 重建计一次恢复 */
        }
        (void)session_on_data(udp, s, &v2, now_ms);
    }
}

static void fec_on_parity(zri_udp_transport_t *udp, zri_session_t *s, const zri_frag_t *v,
                          uint64_t now_ms) {
    if (!udp_fec_enabled()) {
        return;
    }
    if (!s->fec_rx_buf) {
        s->fec_rx_buf = (uint8_t *)malloc(udp->base.frag_bytes);
        if (!s->fec_rx_buf) {
            return;
        }
    }
    if (v->frag_len == 0 || v->frag_len > udp->base.frag_bytes) {
        return;
    }
    memcpy(s->fec_rx_buf, v->chunk, v->frag_len);
    s->fec_rx_len = v->frag_len;
    s->fec_rx_msgid = v->msg_id;
    s->fec_rx_valid = 1;
    fec_try_reconstruct(udp, s, v->msg_id, now_ms);
}

static int session_send(zri_udp_transport_t *udp, zri_session_t *s, const zri_message_t *msg) {
    ntk_udp_dgram_t msgs[TX_BATCH_MAX];
    unsigned nm = 0;
    size_t route_len;
    const uint8_t *data;
    size_t len;
    zri_envelope_t envelope;
    uint8_t prefix[ENVELOPE_HDR_LEN + MAX_ROUTE];
    zri_env_view_t view;
    size_t prefix_len;
    size_t env_len;
    uint32_t nfrag;
    size_t off = 0;
    uint32_t i;
    uint64_t now = zri_now_ms();

    if (!s || !s->used || !msg) {
        return ERR_INVALID;
    }
    route_len = msg->route ? strlen(msg->route) : 0;
    data = (const uint8_t *)msg->payload.data;
    len = msg->payload.len;
    if (len > udp->base.max_msg_bytes || route_len >= MAX_ROUTE) {
        return ERR_TOOBIG;
    }
    if (len > 0 && !data) {
        return ERR_INVALID;
    }

    if (s->txq) {
        /* 上一条消息仍在拥塞排空：返回背压，不覆盖，避免隐性丢包。 */
        udp->base.metrics->backpressure_events++;
        return ERR_AGAIN;
    }
    env_len = zri_envelope_size(route_len, len);
    if (env_len > (size_t)udp->base.max_msg_bytes + MAX_ROUTE + ENVELOPE_HDR_LEN) {
        return ERR_TOOBIG;
    }
    /* 仅把 envelope 头 + route 编进栈上前缀；payload 保持借用，不做整包拷贝。 */
    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = msg->kind;
    envelope.status = msg->status;
    envelope.encoding = msg->payload.encoding;
    envelope.request_id = msg->request_id;
    envelope.route = msg->route;
    envelope.payload = NULL;
    envelope.payload_len = 0;
    if (msg->has_stream) {
        envelope.stream_flags = (uint8_t)(STREAM_FLAG_STREAM | msg->stream.flags);
        envelope.stream_id = msg->stream.stream_id;
        envelope.stream_offset = msg->stream.offset;
        envelope.stream_total_size = msg->stream.total_size;
    }
    prefix_len = zri_envelope_encode(prefix, sizeof(prefix), &envelope);
    if (prefix_len != ENVELOPE_HDR_LEN + route_len) {
        return ERR_INVALID;
    }
    view.prefix = prefix;
    view.prefix_len = prefix_len;
    view.payload = data;
    view.payload_len = len;

    nfrag = env_len ? (uint32_t)((env_len + udp->base.frag_bytes - 1) / udp->base.frag_bytes) : 1;
    for (i = 0; i < nfrag; i++) {
        size_t chunk = env_len - off;
        int last;

        if (chunk > udp->base.frag_bytes) {
            chunk = udp->base.frag_bytes;
        }
        last = (off + chunk) >= env_len;

        if (i < s->cc_burst) {
            if (nm == TX_BATCH_MAX) {
                (void)ntk_socket_sendto_batch(s->sock, msgs, nm);
                nm = 0;
            }
            emit_frag(udp, s, msg->request_id, (uint32_t)env_len, (uint32_t)off, (uint32_t)chunk,
                      &view, last, now, msgs, &nm);
        } else {
            s->txq = (uint8_t *)malloc(env_len);
            if (!s->txq) {
                break;
            }
            env_view_copy(&view, 0, env_len, s->txq);
            s->txq_sent = (uint32_t)off;
            s->txq_total = (uint32_t)env_len;
            s->txq_msgid = msg->request_id;
            break;
        }
        off += chunk;
    }
    if (nm > 0) {
        (void)ntk_socket_sendto_batch(s->sock, msgs, nm);
    }
    if (nfrag >= 2 && s->txq == NULL && udp_fec_enabled()) {
        send_fec(udp, s, &view, env_len, msg->request_id);
    }
    return OK;
}

static void session_txq_drain(zri_udp_transport_t *udp, zri_session_t *s, uint64_t now) {
    ntk_udp_dgram_t msgs[TX_BATCH_MAX];
    unsigned nm = 0;
    uint32_t budget = s->cc_burst;
    zri_env_view_t view;

    if (!s->txq) {
        return;
    }
    /* 排空时 txq 已是连续逻辑缓冲：整段作为 prefix，无独立 payload。 */
    view.prefix = s->txq;
    view.prefix_len = s->txq_total;
    view.payload = NULL;
    view.payload_len = 0;
    while (s->txq_sent < s->txq_total && budget-- > 0) {
        uint32_t off = s->txq_sent;
        uint32_t chunk = s->txq_total - s->txq_sent;
        int last;
        if (chunk > udp->base.frag_bytes) {
            chunk = udp->base.frag_bytes;
        }
        last = (off + chunk) >= s->txq_total;
        if (nm == TX_BATCH_MAX) {
            (void)ntk_socket_sendto_batch(s->sock, msgs, nm);
            nm = 0;
        }
        emit_frag(udp, s, s->txq_msgid, s->txq_total, off, chunk, &view, last, now, msgs, &nm);
        s->txq_sent += chunk;
    }
    if (nm > 0) {
        (void)ntk_socket_sendto_batch(s->sock, msgs, nm);
    }
    if (s->txq_sent >= s->txq_total) {
        free(s->txq);
        s->txq = NULL;
        s->txq_sent = 0;
        s->txq_total = 0;
    }
}

/* ---- 接收 ---- */

/* 解码 envelope 并以统一消息上下文上抛给 core。 */
static void deliver_message(zri_udp_transport_t *udp, zri_session_t *s, const uint8_t *buf,
                            size_t len) {
    zri_envelope_t env;
    zri_message_t msg;
    char route[MAX_ROUTE];

    if (zri_envelope_decode(buf, len, &env) != OK) {
        return;
    }
    zri_envelope_to_message(&env, route, sizeof(route), &msg);
    msg.peer_ip = s->peer_ip_s;
    msg.peer_port = s->peer_port;
    msg.peer_token = s;
    zri_node_on_message(udp->base.node, &udp->base, &msg);
}

static int session_on_data(zri_udp_transport_t *udp, zri_session_t *s, const zri_frag_t *v,
                           uint64_t now_ms) {
    zri_reasm_slot_t *r;
    uint32_t frag_count;
    uint32_t fidx;

    if (!s || !v) {
        return ERR_INVALID;
    }
    if (v->pt == PT_FEC) {
        fec_on_parity(udp, s, v, now_ms);
        return OK;
    }
    s->rx_pkts++;
    /* Peer data arrived: everything we sent so far was received, and we are
     * no longer idling, so reset the sender-side RTO state. */
    s->last_rx_data_ms = now_ms;
    s->acked_upto = s->send_seq;
    s->rto_attempts = 0;
    udp->base.metrics->udp_packets_received++;
    if (!s->ssrc_remote_known) {
        s->ssrc_remote = v->ssrc;
        s->ssrc_remote_known = 1;
    }

    if (!zri_rx_window_accept(&s->rx_window, v->seq)) {
        return OK;
    }

    /* RTCP 接收统计：扩展最高序号 + 抖动估计（RFC 3550 A.8）。 */
    s->rx_received++;
    if (!s->rx_max_seq_set) {
        s->rx_max_seq = v->seq;
        s->rx_max_seq_set = 1;
    } else if ((int16_t)(v->seq - s->rx_max_seq) > 0) {
        if (v->seq < s->rx_max_seq) {
            s->rx_cycles += 65536u; /* 序号回绕 */
        }
        s->rx_max_seq = v->seq;
    }
    {
        int64_t transit = (int64_t)now_ms - (int64_t)v->ts;
        if (s->have_transit) {
            int64_t d = transit - s->transit_prev;
            if (d < 0) {
                d = -d;
            }
            s->jitter += ((double)d - s->jitter) / 16.0;
            if (s->metrics) {
                s->metrics->rtcp_jitter_ms = (uint32_t)s->jitter;
            }
        }
        s->transit_prev = transit;
        s->have_transit = 1;
    }

    if (v->total_len > udp->base.max_msg_bytes + MAX_ROUTE + ENVELOPE_HDR_LEN) {
        return ERR_TOOBIG;
    }
    if ((v->frag_off % udp->base.frag_bytes) != 0) {
        return ERR_INVALID;
    }
    frag_count =
        v->total_len ? (v->total_len + udp->base.frag_bytes - 1) / udp->base.frag_bytes : 1;
    fidx = v->frag_off / udp->base.frag_bytes;
    if (fidx >= frag_count || (uint32_t)v->frag_off + v->frag_len > v->total_len) {
        return ERR_INVALID;
    }

    /* 单分片快路径：解码完整 envelope 后投递。 */
    if (frag_count == 1) {
        deliver_message(udp, s, v->chunk, v->frag_len);
        return OK;
    }

    r = zri_reasm_acquire(&s->reasm, v->msg_id, v->total_len, frag_count, now_ms);
    if (!r) {
        return ERR_NOMEM;
    }
    if (!r->base_seq_set) {
        r->base_seq = (uint16_t)(v->seq - fidx);
        r->base_seq_set = 1;
    }
    (void)zri_reasm_put(r, fidx, v->frag_off, v->chunk, v->frag_len);

    if (r->got == r->frag_count) {
        deliver_message(udp, s, r->buf, r->total_len);
        zri_reasm_release(r);
        return OK;
    }
    /* FEC：若本消息恰好缺一枚分片且已缓存其校验包，尝试重建补齐。 */
    fec_try_reconstruct(udp, s, v->msg_id, now_ms);
    return OK;
}

static void session_on_nack(zri_udp_transport_t *udp, zri_session_t *s, const uint16_t *seqs,
                            size_t n) {
    size_t i;
    if (!s || !seqs) {
        return;
    }
    {
        uint64_t now = zri_now_ms();
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
        const uint8_t *packet = zri_tx_ring_get(&s->tx_ring, seqs[i], &len);
        if (packet) {
            (void)ntk_socket_sendto(s->sock, packet, len, s->peer_ip_s, s->peer_port);
            s->retx++;
            if (s->metrics) {
                s->metrics->udp_retransmissions++;
            }
        }
    }
}

/* ---- RTCP 报告（RFC 3550 SR/RR） ---- */

static uint32_t ntp_msw(uint64_t wall_ms) {
    return (uint32_t)(wall_ms / 1000ull + 2208988800ull); /* 自 1900 起的秒 */
}

static uint32_t ntp_lsw(uint64_t wall_ms) {
    return (uint32_t)(((wall_ms % 1000ull) * 4294967296ull) / 1000ull);
}

/* NTP 时间戳的中间 32 位：秒低 16 位 + 小数高 16 位（用于 LSR/DLSR）。 */
static uint32_t ntp_middle32(uint32_t msw, uint32_t lsw) {
    return (uint32_t)(((msw & 0xffffu) << 16) | (lsw >> 16));
}

static void fill_report_block(const zri_session_t *s, zri_rtcp_report_block_t *rb, uint64_t now_ms) {
    uint32_t ext = s->rx_max_seq_set ? (s->rx_cycles + (uint32_t)s->rx_max_seq) : 0u;
    uint32_t expected = s->rx_max_seq_set ? (ext + 1u) : 0u;
    uint32_t expected_i = expected - s->rx_expected_prior;
    uint32_t received_i = s->rx_received - s->rx_received_prior;
    int32_t lost_i = (int32_t)expected_i - (int32_t)received_i;
    int32_t cum = (int32_t)expected - (int32_t)s->rx_received;

    rb->ssrc = s->ssrc_remote;
    rb->fraction_lost = 0;
    if (expected_i != 0 && lost_i > 0) {
        rb->fraction_lost = (uint8_t)(((uint32_t)lost_i << 8) / expected_i);
    }
    if (cum < -0x800000) {
        cum = -0x800000;
    } else if (cum > 0x7fffff) {
        cum = 0x7fffff;
    }
    rb->cumulative_lost = cum;
    rb->ext_highest_seq = ext;
    rb->jitter = (uint32_t)s->jitter;
    rb->lsr = s->have_last_rx_sr ? s->last_rx_sr_ntp : 0u;
    rb->dlsr = s->have_last_rx_sr
                   ? (uint32_t)(((now_ms - s->last_rx_sr_ms) * 65536ull) / 1000ull)
                   : 0u;
}

/* 从对端报告块中提取关于本端流（ssrc_local）的丢包/抖动/RTT。 */
static void apply_report_blocks(zri_session_t *s, const zri_rtcp_report_block_t *rb, int n) {
    uint32_t now32 = ntp_middle32(ntp_msw(ntk_wall_ms()), ntp_lsw(ntk_wall_ms()));
    int i;
    for (i = 0; i < n; i++) {
        if (rb[i].ssrc != s->ssrc_local) {
            continue;
        }
        s->rtcp_remote_lost = rb[i].cumulative_lost;
        if (s->metrics) {
            s->metrics->rtcp_remote_lost = rb[i].cumulative_lost;
        }
        if (rb[i].lsr != 0) {
            /* RTT = now - LSR - DLSR，单位 1/65536 秒。 */
            uint32_t d = now32 - rb[i].lsr - rb[i].dlsr;
            uint32_t rtt_ms = (uint32_t)(((uint64_t)d * 1000ull) >> 16);
            s->last_rtt_ms = rtt_ms;
            if (s->metrics) {
                s->metrics->rtcp_rtt_ms = rtt_ms;
            }
        }
    }
}

static void session_on_rtcp(zri_udp_transport_t *udp, zri_session_t *s, const uint8_t *buf,
                            size_t n, uint64_t now_ms) {
    (void)udp;
    if (!s || !buf || n < 4) {
        return;
    }
    if (buf[1] == RTCP_SR) {
        zri_rtcp_sr_t sr;
        if (zri_frame_parse_sr(buf, n, &sr) == 1) {
            s->rtcp_sr_received++;
            if (s->metrics) {
                s->metrics->rtcp_sr_received++;
            }
            s->ssrc_remote = sr.ssrc;
            s->ssrc_remote_known = 1;
            s->last_rx_sr_ntp = ntp_middle32(sr.ntp_msw, sr.ntp_lsw);
            s->last_rx_sr_ms = now_ms;
            s->have_last_rx_sr = 1;
            apply_report_blocks(s, sr.rb, sr.rb_count);
        }
    } else if (buf[1] == RTCP_RR) {
        zri_rtcp_rr_t rr;
        if (zri_frame_parse_rr(buf, n, &rr) == 1) {
            s->rtcp_rr_received++;
            if (s->metrics) {
                s->metrics->rtcp_rr_received++;
            }
            if (!s->ssrc_remote_known) {
                s->ssrc_remote = rr.ssrc;
                s->ssrc_remote_known = 1;
            }
            apply_report_blocks(s, rr.rb, rr.rb_count);
        }
    }
}

/* 周期发送复合 RTCP 包：本端有发送记录则 SR，否则 RR；尾部附 SDES CNAME。 */
static void session_send_rtcp(zri_udp_transport_t *udp, zri_session_t *s, uint64_t now_ms) {
    uint8_t buf[256];
    zri_rtcp_report_block_t rb;
    int have_rx = s->ssrc_remote_known && s->rx_pkts > 0;
    size_t n = 0;
    size_t total;
    (void)udp;

    memset(&rb, 0, sizeof(rb));
    if (have_rx) {
        fill_report_block(s, &rb, now_ms);
    }
    if (s->rtcp_pkt_count > 0) {
        uint32_t msw = ntp_msw(s->rtcp_last_rtp_wall);
        uint32_t lsw = ntp_lsw(s->rtcp_last_rtp_wall);
        n = zri_frame_build_sr(buf, sizeof(buf), s->ssrc_local, msw, lsw, s->rtcp_last_rtp_ts,
                                s->rtcp_pkt_count, s->rtcp_octet_count, have_rx ? &rb : NULL,
                                have_rx ? 1 : 0);
        if (n > 0) {
            s->rtcp_last_sr_ntp = ntp_middle32(msw, lsw);
            s->rtcp_sr_sent++;
            if (s->metrics) {
                s->metrics->rtcp_sr_sent++;
            }
        }
    } else {
        n = zri_frame_build_rr(buf, sizeof(buf), s->ssrc_local, have_rx ? &rb : NULL,
                                have_rx ? 1 : 0);
        if (n > 0) {
            s->rtcp_rr_sent++;
            if (s->metrics) {
                s->metrics->rtcp_rr_sent++;
            }
        }
    }
    if (n == 0) {
        return;
    }
    total = n + zri_frame_build_sdes_cname(buf + n, sizeof(buf) - n, s->ssrc_local, "rpc");
    (void)ntk_socket_sendto(s->sock, buf, total, s->peer_ip_s, s->peer_port);
    s->rx_expected_prior = s->rx_max_seq_set ? (s->rx_cycles + s->rx_max_seq + 1u) : 0u;
    s->rx_received_prior = s->rx_received;
}

/* 会话结束时尽力发送 RTCP BYE，通知对端本端要离开。 */
static void session_send_bye(zri_udp_transport_t *udp, zri_session_t *s) {
    uint8_t buf[16];
    size_t n;
    (void)udp;
    if (!s || !s->sock) {
        return;
    }
    n = zri_frame_build_bye(buf, sizeof(buf), s->ssrc_local);
    if (n == 0) {
        return;
    }
    (void)ntk_socket_sendto(s->sock, buf, n, s->peer_ip_s, s->peer_port);
    if (s->metrics) {
        s->metrics->rtcp_bye_sent++;
    }
}

static void reasm_on_lost(const zri_reasm_slot_t *slot, void *user) {
    zri_session_t *s = (zri_session_t *)user;
    (void)slot;
    if (s) {
        s->lost++;
        if (s->metrics) {
            s->metrics->udp_reassembly_expired++;
        }
    }
}

/* Sender-side per-message RTO (Plan A): when the session has been idle (no peer
 * data) for >= RTO, retransmit the unacked tail. This recovers a lost last
 * fragment that the peer rx_window cannot flag (no higher seq => no NACK).
 * RTO is overridable via env RTO_MS (ms), default 100. */
#define RTO_MAX_ROUNDS 6

static uint32_t udp_rto_env_ms(void) {
    static int inited = 0;
    static uint32_t value = 0;
    if (!inited) {
        const char *env = getenv("RTO_MS");
        if (env && env[0]) {
            int v = atoi(env);
            if (v > 0 && v < 5000) {
                value = (uint32_t)v;
            }
        }
        inited = 1;
    }
    return value;
}

/* Sender-side RTO. Default 10ms (LAN/loopback/nearby); override with
 * RTO_MS for high-RTT WAN links. (RTCP-derived RTT proved too coarse to
 * drive this adaptively on a low-latency link.) */
static uint32_t session_rto_ms(const zri_session_t *s) {
    uint32_t env = udp_rto_env_ms();
    (void)s;
    return env > 0 ? env : 10u;
}

static void session_tick(zri_udp_transport_t *udp, zri_session_t *s, uint64_t now_ms) {
    uint16_t missing[MAX_NACK_SEQS];
    size_t mn = 0;

    if (!s || !s->used) {
        return;
    }

    session_txq_drain(udp, s, now_ms);

    /* Plan A: idle sender-side RTO retransmit of the unacked tail. */
    if (s->tx_pkts > 0 && !s->txq && s->rto_attempts < RTO_MAX_ROUNDS &&
        (now_ms - s->last_rx_data_ms) >= session_rto_ms(s) &&
        (int16_t)(s->send_seq - s->acked_upto) > 0) {
        uint32_t rto = session_rto_ms(s);
        unsigned i;
        for (i = 0; i < TX_BATCH_MAX; i++) {
            uint16_t seq = (uint16_t)(s->send_seq - 1u - i);
            const uint8_t *pkt;
            uint64_t sent;
            uint32_t len = 0;
            if ((int16_t)(seq - s->acked_upto) < 0) {
                break;
            }
            sent = zri_tx_ring_sent_ms(&s->tx_ring, seq);
            if (sent == 0 || now_ms - sent < rto) {
                continue;
            }
            pkt = zri_tx_ring_get(&s->tx_ring, seq, &len);
            if (!pkt || len == 0) {
                continue;
            }
            (void)ntk_socket_sendto(s->sock, pkt, len, s->peer_ip_s, s->peer_port);
            zri_tx_ring_mark_sent(&s->tx_ring, seq, now_ms);
            s->retx++;
            if (s->metrics) {
                s->metrics->udp_retransmissions++;
            }
        }
        s->rto_attempts++;
    }

    if (s->cc_burst < s->cc_burst_max && now_ms - s->cc_last_loss_ms > 200 &&
        now_ms - s->cc_last_grow_ms > 50) {
        s->cc_burst++;
        s->cc_last_grow_ms = now_ms;
    }

    mn += zri_rx_window_missing(&s->rx_window, missing + mn, MAX_NACK_SEQS - mn);
    mn += zri_reasm_missing(&s->reasm, missing + mn, MAX_NACK_SEQS - mn);

    if (mn > 0) {
        if (s->first_gap_ms == 0) {
            s->first_gap_ms = now_ms;
        }
        if (now_ms - s->last_nack_ms >= NACK_INTERVAL_MS) {
            uint8_t nack[256];
            size_t n = zri_frame_build_nack(nack, sizeof(nack), s->ssrc_local, s->ssrc_remote,
                                            missing, mn);
            if (n > 0) {
                (void)ntk_socket_sendto(s->sock, nack, n, s->peer_ip_s, s->peer_port);
                s->last_nack_ms = now_ms;
                if (s->metrics) {
                    s->metrics->udp_nacks_sent++;
                }
            }
        }
    } else {
        s->first_gap_ms = 0;
    }

    if (!udp->base.rtcp_disabled && (s->rx_pkts > 0 || s->tx_pkts > 0) &&
        now_ms - s->rtcp_last_send_ms >=
            (udp->base.rtcp_interval_ms ? udp->base.rtcp_interval_ms : RTCP_INTERVAL_MS)) {
        session_send_rtcp(udp, s, now_ms);
        s->rtcp_last_send_ms = now_ms;
    }

    zri_reasm_expire(&s->reasm, now_ms, REASM_TIMEOUT_MS, reasm_on_lost, s);
}

/* ---- 数据面接收 ---- */

static void data_recv_cb(ntk_socket *sock, void *user) {
    zri_udp_io_ctx_t *ctx = (zri_udp_io_ctx_t *)user;
    zri_udp_transport_t *udp = ctx ? ctx->udp : NULL;
    int io = ctx ? ctx->io : 0;
    ntk_udp_recv_t recvs[RX_BATCH_MAX];

    if (!udp || !ctx->rx) {
        return;
    }
    for (;;) {
        unsigned i;
        int got;
        for (i = 0; i < RX_BATCH_MAX; i++) {
            recvs[i].data = ctx->rx + (size_t)i * RTX_SLOT_MAX;
            recvs[i].cap = RTX_SLOT_MAX;
            recvs[i].len = 0;
            recvs[i].ip[0] = '\0';
            recvs[i].port = 0;
        }
        got = ntk_socket_recvfrom_batch(sock, recvs, RX_BATCH_MAX);
        if (got <= 0) {
            break;
        }
        {
        uint64_t now = zri_now_ms();
        for (i = 0; i < (unsigned)got; i++) {
            uint8_t *buf = (uint8_t *)recvs[i].data;
            size_t n = recvs[i].len;
            if (n < 2) {
                continue;
            }
            if (buf[1] == RTCP_SR || buf[1] == RTCP_RR) {
                zri_session_t *s = session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                if (s) {
                    session_on_rtcp(udp, s, buf, n, now);
                }
                continue;
            }
            if (buf[1] == RTCP_BYE) {
                zri_session_t *s = session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                if (s) {
                    s->ssrc_remote_known = 0;
                    s->have_last_rx_sr = 0;
                    if (s->metrics) {
                        s->metrics->rtcp_bye_received++;
                    }
                }
                continue;
            }
            if ((buf[1] & 0x7f) == RTCP_RTPFB) {
                uint16_t seqs[MAX_NACK_SEQS];
                int c = zri_frame_parse_nack(buf, n, NULL, NULL, seqs, MAX_NACK_SEQS);
                if (c > 0) {
                    zri_session_t *s = session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                    if (s) {
                        session_on_nack(udp, s, seqs, (size_t)c);
                    }
                }
                continue;
            }
            {
                zri_frag_t v;
                int r = zri_frame_parse_data(buf, n, &v);
                if (r == 1) {
                    zri_session_t *s = session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
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
    zri_udp_io_ctx_t *ctx = (zri_udp_io_ctx_t *)user;
    zri_udp_transport_t *udp = ctx->udp;
    uint64_t now = zri_now_ms();
    int i;
    for (i = 0; i < PEERS_PER_IO; i++) {
        session_tick(udp, &udp->sessions[ctx->io][i], now);
    }
}

/* ---- 传输接口 ---- */

static int udp_start(zri_transport_t *t) {
    zri_udp_transport_t *udp = (zri_udp_transport_t *)t;
    ntk_socket_callbacks_t cbs;
    uint16_t actual = 0;
    int i;

    udp->io_count = 0;
    for (i = 0; i < (int)t->io_threads && i < IO_MAX; i++) {
        ntk_socket *ds = ntk_socket_create();
        if (!ds) {
            break;
        }
        if (ntk_socket_bind_udp_ex(ds, t->bind_host, t->port, 1, i > 0 ? 1 : 0) != NTK_OK) {
            ntk_socket_destroy(ds);
            break;
        }
        if (i == 0 && ntk_socket_get_local(ds, NULL, 0, &actual) == NTK_OK) {
            t->port = actual;
        }
#if !defined(_WIN32)
        {
            int fd = ntk_socket_fd(ds);
            int sz = 8 * 1024 * 1024;
            if (fd >= 0) {
                (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, (socklen_t)sizeof(sz));
                (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, (socklen_t)sizeof(sz));
            }
        }
#endif
        udp->io_ctx[i].udp = udp;
        udp->io_ctx[i].io = i;
        udp->io_ctx[i].rx = (uint8_t *)malloc((size_t)RX_BATCH_MAX * RTX_SLOT_MAX);
        if (!udp->io_ctx[i].rx) {
            ntk_socket_destroy(ds);
            break;
        }
        memset(&cbs, 0, sizeof(cbs));
        cbs.on_readable = data_recv_cb;
        udp->pollers[i] = ntk_poller_pool_at(t->pool, (unsigned)i);
        if (ntk_socket_attach_poller(ds, udp->pollers[i], &cbs, &udp->io_ctx[i]) != NTK_OK) {
            free(udp->io_ctx[i].rx);
            udp->io_ctx[i].rx = NULL;
            ntk_socket_destroy(ds);
            break;
        }
        udp->socks[i] = ds;
        udp->io_count++;
    }
    if (udp->io_count == 0) {
        return ERR_IO;
    }
    for (i = 0; i < udp->io_count; i++) {
        udp->tick_timers[i] = ntk_timer_start(udp->pollers[i], zri_tick_interval_ms(), 1,
                                              udp_tick_cb, &udp->io_ctx[i]);
    }
    return OK;
}

static void udp_stop(zri_transport_t *t) {
    zri_udp_transport_t *udp = (zri_udp_transport_t *)t;
    int i, j;
    for (i = 0; i < IO_MAX; i++) {
        if (udp->tick_timers[i]) {
            ntk_timer_stop(udp->tick_timers[i]);
            udp->tick_timers[i] = NULL;
        }
    }
    /* socket 仍在：先尽力通知所有对端本端离开。 */
    for (i = 0; i < IO_MAX; i++) {
        if (!udp->socks[i]) {
            continue;
        }
        for (j = 0; j < PEERS_PER_IO; j++) {
            if (udp->sessions[i][j].used) {
                session_send_bye(udp, &udp->sessions[i][j]);
            }
        }
    }
    for (i = 0; i < IO_MAX; i++) {
        if (udp->socks[i]) {
            ntk_socket_detach_poller(udp->socks[i]);
            ntk_socket_destroy(udp->socks[i]);
            udp->socks[i] = NULL;
        }
        if (udp->io_ctx[i].rx) {
            free(udp->io_ctx[i].rx);
            udp->io_ctx[i].rx = NULL;
        }
        for (j = 0; j < PEERS_PER_IO; j++) {
            if (udp->sessions[i][j].used) {
                session_cleanup(&udp->sessions[i][j]);
                udp->sessions[i][j].used = 0;
            }
        }
    }
}

static int udp_send(zri_transport_t *t, const zri_message_t *msg) {
    zri_udp_transport_t *udp = (zri_udp_transport_t *)t;
    zri_session_t *s;
    if (!msg || !msg->peer_ip || !msg->peer_port || udp->io_count == 0) {
        return ERR_INVALID;
    }
    s = session_for(udp, 0, udp->socks[0], msg->peer_ip, msg->peer_port, zri_now_ms());
    if (!s) {
        return ERR_NOMEM;
    }
    return session_send(udp, s, msg);
}

static int udp_reply(zri_transport_t *t, const zri_message_t *msg) {
    zri_udp_transport_t *udp = (zri_udp_transport_t *)t;
    zri_session_t *s;
    if (!msg) {
        return ERR_INVALID;
    }
    s = (zri_session_t *)msg->peer_token;
    if (!s || !s->used) {
        if (!msg->peer_ip || !msg->peer_port || udp->io_count == 0) {
            return ERR_INVALID;
        }
        s = session_for(udp, 0, udp->socks[0], msg->peer_ip, msg->peer_port, zri_now_ms());
        if (!s) {
            return ERR_NOMEM;
        }
    }
    return session_send(udp, s, msg);
}

static int udp_peer_stats(zri_transport_t *t, const char *ip, uint16_t port,
                          peer_stats_t *out) {
    zri_udp_transport_t *udp = (zri_udp_transport_t *)t;
    int io;
    int i;
    if (!udp || !ip || !out) {
        return -1;
    }
    for (io = 0; io < udp->io_count; io++) {
        for (i = 0; i < PEERS_PER_IO; i++) {
            zri_session_t *s = &udp->sessions[io][i];
            if (s->used && s->peer_port == port && strcmp(s->peer_ip_s, ip) == 0) {
                out->local_id = s->ssrc_local;
                out->remote_id = s->ssrc_remote;
                out->rtt_ms = s->last_rtt_ms;
                out->jitter_ms = (uint32_t)s->jitter;
                out->lost = s->rtcp_remote_lost;
                out->feedback_sent = s->rtcp_sr_sent + s->rtcp_rr_sent;
                out->feedback_received = s->rtcp_sr_received + s->rtcp_rr_received;
                return 1;
            }
        }
    }
    return 0;
}

static const zri_transport_ops_t g_udp_ops = {
    udp_start,
    udp_stop,
    udp_send,
    udp_reply,
    udp_peer_stats,
};

zri_transport_t *zri_udp_transport_create(void) {
    zri_udp_transport_t *udp = (zri_udp_transport_t *)calloc(1, sizeof(*udp));
    if (!udp) {
        return NULL;
    }
    udp->base.ops = &g_udp_ops;
    udp->base.kind = TRANSPORT_UDP;
    zri_copy_str(udp->base.scheme, sizeof(udp->base.scheme), "udp", "udp");
    return &udp->base;
}
