/*
 * UDP 传输驱动：RTP 承载 + 分片/重组 + 乱序窗口 + NACK/RTX + AIMD。
 *
 * 每个 io 分片有独立的 socket 与会话表（钉核，无锁）；发现面不在这里。
 * 上层通过 nb_node_on_message 收到完整消息。
 */
#include "transport_internal.h"

#include "atomic.h"
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

/* 出站拥塞排空队列节点：一条尚未发完的消息（连续逻辑缓冲）。 */
typedef struct nb_udp_txq {
    struct nb_udp_txq *next;
    uint8_t *buf;
    uint32_t sent;
    uint32_t total;
    uint32_t msgid;
} nb_udp_txq_t;

/* 每个对端一个会话（仅在所属 io 分片上访问，无锁） */
typedef struct nb_session {
    int used;
    int io;
    nb_metrics_t *metrics;
    ntk_socket *sock;
    uint32_t peer_ip;
    char peer_ip_s[NB_ENDPOINT_MAX];
    uint16_t peer_port;
    uint64_t last_use_ms;
    uint32_t ssrc_local;
    uint32_t ssrc_remote;
    int ssrc_remote_known;
    uint16_t send_seq;
    uint64_t last_rx_data_ms; /* recv-side: last time peer data arrived (sender RTO) */
    uint16_t acked_upto;      /* highest send seq inferred received by peer */
    uint8_t rto_attempts;     /* idle-RTO retransmit rounds; reset on peer data */

    nb_rx_window_t rx_window;
    uint64_t last_nack_ms;
    uint64_t first_gap_ms;

    nb_tx_ring_t tx_ring;

    nb_udp_txq_t *txq_head; /* 拥塞排队消息（FIFO）；backpressure 按字节高水位 */
    nb_udp_txq_t *txq_tail;
    uint32_t txq_pending_bytes; /* 队列中尚未发送的字节数 */
    uint32_t cc_burst;
    uint32_t cc_burst_max;
    uint64_t cc_last_loss_ms;
    uint64_t cc_last_grow_ms;

    nb_reasm_t reasm;

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
    uint32_t rtcp_pkt_count;     /* 本端发出的 RTP 包数（供 SR） */
    uint32_t rtcp_octet_count;   /* 本端发出的 RTP payload 字节数（供 SR） */
    uint32_t rtcp_last_rtp_ts;   /* 最近发送的 RTP 时间戳 */
    uint64_t rtcp_last_rtp_wall; /* 该时间戳对应的墙钟毫秒 */
    uint32_t rtcp_last_sr_ntp;   /* 本端最近发出 SR 的 NTP middle32 */
    int rx_max_seq_set;
    uint16_t rx_max_seq;
    uint32_t rx_cycles; /* 序号回绕计数 */
    uint32_t rx_received;
    uint32_t rx_expected_prior; /* 上次报告时的期望包数 */
    uint32_t rx_received_prior; /* 上次报告时的实收包数 */
    double jitter;              /* 接收抖动估计（毫秒） */
    int have_transit;
    int64_t transit_prev;
    uint32_t last_rx_sr_ntp; /* 对端最近 SR 的 NTP middle32 */
    uint64_t last_rx_sr_ms;  /* 收到该 SR 的本端单调时刻 */
    int have_last_rx_sr;
    uint32_t last_rtt_ms;     /* 由对端报告块测得 */
    int32_t rtcp_remote_lost; /* 对端报告的本端流累计丢包 */
    uint64_t rtcp_sr_sent;    /* 本会话 SR/RR 收发明细（供按对端查询） */
    uint64_t rtcp_rr_sent;
    uint64_t rtcp_sr_received;
    uint64_t rtcp_rr_received;
} nb_session_t;

typedef struct nb_udp_io_ctx {
    struct nb_udp_transport *udp;
    int io;
    uint8_t *rx;
} nb_udp_io_ctx_t;

typedef struct nb_udp_transport {
    nb_transport_t base;
    ntk_socket *socks[IO_MAX];
    ntk_poller *pollers[IO_MAX];
    ntk_timer *tick_timers[IO_MAX];
    nb_udp_io_ctx_t io_ctx[IO_MAX];
    nb_session_t sessions[IO_MAX][PEERS_PER_IO];
    int io_count;
} nb_udp_transport_t;

/* 伪随机：会话可能在任意 poller 上创建，状态必须无数据竞争。 */
static uint32_t nb_xorshift_next(uint32_t *state)
{
    uint32_t cur = nb_atomic_load_u32(state);
    if (cur == 0) {
        cur = (uint32_t)ntk_monotonic_ms() ^ 0x9e3779b9u;
        if (cur == 0) {
            cur = 0x9e3779b9u;
        }
    }
    for (;;) {
        uint32_t next = cur;
        next ^= next << 13;
        next ^= next >> 17;
        next ^= next << 5;
        if (next == 0) {
            next = 1;
        }
        if (nb_atomic_cas_u32(state, &cur, next)) {
            return next;
        }
    }
}

/* 丢包注入用的确定性伪随机 */
static uint32_t nb_sess_rand(void)
{
    static uint32_t st = 0x9e3779b9u;
    return nb_xorshift_next(&st);
}

static uint32_t nb_udp_rand(void)
{
    static uint32_t st;
    return nb_xorshift_next(&st);
}

static void nb_session_cleanup(nb_session_t *s)
{
    nb_udp_txq_t *q;
    if (!s) {
        return;
    }
    if (s->used && s->metrics) {
        NB_METRIC_SUB(s->metrics->connections, 1u);
    }
    s->used = 0;
    nb_tx_ring_cleanup(&s->tx_ring);
    q = s->txq_head;
    while (q) {
        nb_udp_txq_t *next = q->next;
        free(q->buf);
        free(q);
        q = next;
    }
    s->txq_head = NULL;
    s->txq_tail = NULL;
    s->txq_pending_bytes = 0;
    if (s->fec_rx_buf) {
        free(s->fec_rx_buf);
        s->fec_rx_buf = NULL;
    }
    nb_reasm_reset(&s->reasm);
    nb_rx_window_reset(&s->rx_window);
}

static void nb_session_send_bye(nb_udp_transport_t *udp, nb_session_t *s);

static void nb_session_init(nb_udp_transport_t *udp,
                         nb_session_t *s,
                         int io,
                         ntk_socket *sock,
                         const char *ip_s,
                         uint16_t port,
                         uint64_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->io = io;
    s->metrics = udp->base.metrics;
    s->sock = sock;
    nb_rx_window_reset(&s->rx_window);
    nb_reasm_init(&s->reasm);
    nb_reasm_set_budget(&s->reasm, udp->base.bp_high_water);
    s->cc_burst_max = 1024;
    s->cc_burst = 16;
    s->peer_port = port;
    nb_copy_str(s->peer_ip_s, sizeof(s->peer_ip_s), ip_s, "0.0.0.0");
    s->ssrc_local = nb_udp_rand();
    if (s->ssrc_local == 0) {
        s->ssrc_local = 1;
    }
    s->last_use_ms = now_ms;
    s->last_rx_data_ms = now_ms;
    s->rtcp_last_send_ms = now_ms;
    if (udp->base.metrics) {
        NB_METRIC_INC(udp->base.metrics->connections);
    }
}

/*
 * 查找/创建某 io 分片上的对端会话。表满时淘汰“最久未使用”的会话，
 * 而不是直接丢包——否则并发对端数超过 PEERS_PER_IO 时，新对端的首请求
 * 会被静默丢弃且永远建不起会话（表现为首请求超时）。
 */
static nb_session_t *nb_session_for(nb_udp_transport_t *udp,
                                  int io,
                                  ntk_socket *sock,
                                  const char *ip_s,
                                  uint16_t port,
                                  uint64_t now_ms)
{
    int i;
    int free_slot = -1;
    int lru_slot = -1;
    uint64_t lru_ms = 0;

    if (io < 0 || io >= IO_MAX) {
        return NULL;
    }
    for (i = 0; i < PEERS_PER_IO; i++) {
        nb_session_t *s = &udp->sessions[io][i];
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
        nb_session_t *s = &udp->sessions[io][slot];
        if (free_slot < 0) {
            nb_session_send_bye(udp, s); /* 淘汰前通知对端 */
            nb_session_cleanup(s);       /* 释放被淘汰会话的动态缓冲 */
        }
        nb_session_init(udp, s, io, sock, ip_s, port, now_ms);
        return s;
    }
}

/* ---- 发送 ---- */

/*
 * 逻辑 envelope 视图：[prefix = envelope 头 + route][payload]。
 * prefix 常驻栈/池，payload 借用调用方内存，避免整包 envbuf 的分配与拷贝。
 */
typedef struct nb_env_view {
    const uint8_t *prefix;
    size_t prefix_len;
    const uint8_t *payload;
    size_t payload_len;
} nb_env_view_t;

/* 从逻辑视图的 off 处拷贝 len 字节到 dst。 */
static void nb_env_view_copy(const nb_env_view_t *v, size_t off, size_t len, uint8_t *dst)
{
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

/*
 * 批量发送可能短写（非阻塞 socket / 发送缓冲瞬时不足），sendmmsg 只发出一部分。
 * 剩余分片必须补发，否则接收端要等到下一个 tick 才 NACK，9 个分片以上会出现
 * 明显的 ~2ms 台阶。这里有限次重试，仍失败则交给 NACK/RTO 兜底。
 */
static void nb_udp_send_msgs(nb_session_t *s, ntk_udp_dgram_t *msgs, unsigned nm)
{
    unsigned off = 0;
    unsigned spins = 0;
    while (s && off < nm) {
        int n = ntk_socket_sendto_batch(s->sock, msgs + off, nm - off);
        if (n <= 0) {
            if (++spins >= 4) {
                break;
            }
            continue;
        }
        off += (unsigned)n;
        spins = 0;
    }
}

static void nb_emit_frag(nb_udp_transport_t *udp,
                      nb_session_t *s,
                      uint32_t msg_id,
                      uint32_t total_len,
                      uint32_t off,
                      uint32_t chunk,
                      const nb_env_view_t *view,
                      int last,
                      uint64_t now,
                      ntk_udp_dgram_t *msgs,
                      unsigned *nm)
{
    nb_frag_t d;
    uint8_t *dst;
    size_t hdr;
    size_t n;
    uint32_t stride = RTP_HDR_LEN + FRAG_HDR_LEN + udp->base.frag_bytes;

    if (!nb_tx_ring_ensure(&s->tx_ring, stride)) {
        return;
    }
    dst = nb_tx_ring_slot(&s->tx_ring, s->send_seq);

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
    hdr = nb_frame_build_frag_header(dst, stride, &d);
    if (hdr == 0) {
        return;
    }
    nb_env_view_copy(view, off, chunk, dst + hdr);
    n = hdr + chunk;
    nb_tx_ring_commit(&s->tx_ring, s->send_seq, (uint32_t)n);
    nb_tx_ring_mark_sent(&s->tx_ring, s->send_seq, now);
    /* 测试用丢包注入：仍填入重传环（NACK 可重传），但不进本批发送 */
    if (!(udp->base.drop_percent && (nb_sess_rand() % 100u) < udp->base.drop_percent)) {
        msgs[*nm].data = dst;
        msgs[*nm].len = n;
        msgs[*nm].ip = s->peer_ip_s;
        msgs[*nm].port = s->peer_port;
        (*nm)++;
    }
    s->send_seq++;
    s->tx_pkts++;
    NB_METRIC_INC(udp->base.metrics->udp_packets_sent);
    /* RTCP Sender Report 的收发计数与时间戳映射 */
    s->rtcp_pkt_count++;
    s->rtcp_octet_count += chunk;
    s->rtcp_last_rtp_ts = d.ts;
    s->rtcp_last_rtp_wall = ntk_wall_ms();
}

/* ---- 整包 XOR 前向纠错（env FEC=1，默认关） ----
 * 发送端为每个多分片消息额外发 1 个 XOR 校验包；接收端若该消息恰好缺 1 个分片，
 * 可用校验包 XOR 其余已收分片重建它，省掉一次 NACK/RTX 往返。 */

static int nb_session_on_data(nb_udp_transport_t *udp,
                           nb_session_t *s,
                           const nb_frag_t *v,
                           uint64_t now_ms);

static int nb_udp_fec_enabled(void)
{
    static int inited = 0;
    static int on = 0;
    if (!inited) {
        const char *e = getenv("FEC");
        on = (e && e[0] == '1') ? 1 : 0;
        inited = 1;
    }
    return on;
}

static void nb_send_fec(nb_udp_transport_t *udp,
                     nb_session_t *s,
                     const nb_env_view_t *view,
                     size_t env_len,
                     uint32_t msg_id)
{
    uint8_t parity[FRAG_MAX];
    uint8_t pkt[RTP_HDR_LEN + FRAG_HDR_LEN + FRAG_MAX];
    size_t frag = udp->base.frag_bytes;
    size_t parity_len = env_len < frag ? env_len : frag;
    size_t p, hdr;
    nb_frag_t d;

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
    d.ts = (uint32_t)nb_now_ms();
    d.ssrc = s->ssrc_local;
    d.msg_id = msg_id;
    d.total_len = (uint32_t)env_len;
    d.frag_off = 0;
    d.frag_len = (uint32_t)parity_len;
    hdr = nb_frame_build_frag_header(pkt, sizeof(pkt), &d);
    if (hdr == 0) {
        return;
    }
    memcpy(pkt + hdr, parity, parity_len);
    (void)ntk_socket_sendto(s->sock, pkt, hdr + parity_len, s->peer_ip_s, s->peer_port);
}

static void nb_fec_try_reconstruct(nb_udp_transport_t *udp,
                                nb_session_t *s,
                                uint32_t msg_id,
                                uint64_t now_ms)
{
    nb_reasm_slot_t *slot;
    uint32_t frag = udp->base.frag_bytes;
    uint32_t total, frag_count, m = 0, idx, k;
    uint8_t acc[FRAG_MAX];
    int missing = -1;
    nb_frag_t v2;

    if (!nb_udp_fec_enabled() || !s->fec_rx_valid || s->fec_rx_msgid != msg_id || !s->fec_rx_buf ||
        frag == 0) {
        return;
    }
    slot = nb_reasm_find(&s->reasm, msg_id);
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
            NB_METRIC_INC(s->metrics->udp_retransmissions); /* FEC 重建计一次恢复 */
        }
        (void)nb_session_on_data(udp, s, &v2, now_ms);
    }
}

static void nb_fec_on_parity(nb_udp_transport_t *udp,
                          nb_session_t *s,
                          const nb_frag_t *v,
                          uint64_t now_ms)
{
    if (!nb_udp_fec_enabled()) {
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
    nb_fec_try_reconstruct(udp, s, v->msg_id, now_ms);
}

static int nb_session_send(nb_udp_transport_t *udp, nb_session_t *s, const nb_message_t *msg)
{
    ntk_udp_dgram_t msgs[TX_BATCH_MAX];
    unsigned nm = 0;
    size_t route_len;
    const uint8_t *data;
    size_t len;
    nb_envelope_t envelope;
    uint8_t prefix[ENVELOPE_HDR_LEN + MAX_ENVELOPE_EXTRA];
    nb_env_view_t view;
    size_t prefix_len;
    size_t env_len;
    uint32_t nfrag;
    size_t off = 0;
    uint32_t i;
    uint64_t now = nb_now_ms();

    if (!s || !s->used || !msg) {
        return NB_ERR_INVALID;
    }
    route_len = msg->route ? strlen(msg->route) : 0;
    data = (const uint8_t *)msg->payload.data;
    len = msg->payload.len;
    if (len > udp->base.max_msg_bytes || route_len >= MAX_ROUTE) {
        return NB_ERR_TOOBIG;
    }
    if (len > 0 && !data) {
        return NB_ERR_INVALID;
    }

    /* 仅把 envelope 头 + route 编进栈上前缀；payload 保持借用，不做整包拷贝。 */
    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = msg->kind;
    envelope.flags = msg->flags;
    envelope.status = msg->status;
    envelope.encoding = msg->payload.encoding;
    envelope.request_id = msg->request_id;
    envelope.timeout_ms = msg->timeout_ms;
    envelope.route = msg->route;
    envelope.metadata = msg->metadata;
    envelope.metadata_len = msg->metadata_len;
    envelope.status_msg = msg->status_msg;
    envelope.status_msg_len = msg->status_msg_len;
    envelope.payload = NULL;
    envelope.payload_len = len;
    if (msg->has_stream) {
        envelope.stream_flags = (uint8_t)(STREAM_FLAG_STREAM | msg->stream.flags);
        envelope.stream_id = msg->stream.stream_id;
        envelope.stream_offset = msg->stream.offset;
        envelope.stream_total_size = msg->stream.total_size;
    }
    env_len = nb_envelope_size(&envelope);
    if (env_len == 0 || env_len > (size_t)udp->base.max_msg_bytes + MAX_ENVELOPE_EXTRA) {
        return NB_ERR_TOOBIG;
    }
    if (s->txq_head && s->txq_pending_bytes + env_len > udp->base.bp_high_water) {
        /* 出站拥塞队列超过 backpressure_bytes：返回背压，交给上层退避重试。 */
        NB_METRIC_INC(udp->base.metrics->backpressure_events);
        return NB_ERR_AGAIN;
    }
    prefix_len = nb_envelope_encode_prefix(prefix, sizeof(prefix), &envelope);
    if (prefix_len == 0 || prefix_len + len != env_len) {
        return NB_ERR_INVALID;
    }
    view.prefix = prefix;
    view.prefix_len = prefix_len;
    view.payload = data;
    view.payload_len = len;

    nfrag = env_len ? (uint32_t)((env_len + udp->base.frag_bytes - 1) / udp->base.frag_bytes) : 1;
    if (nfrag > MAX_FRAGMENTS) {
        return NB_ERR_TOOBIG;
    }
    for (i = 0; i < nfrag; i++) {
        size_t chunk = env_len - off;
        int last;

        if (chunk > udp->base.frag_bytes) {
            chunk = udp->base.frag_bytes;
        }
        last = (off + chunk) >= env_len;

        if (i < s->cc_burst && s->txq_head == NULL) {
            if (nm == TX_BATCH_MAX) {
                nb_udp_send_msgs(s, msgs, nm);
                nm = 0;
            }
            nb_emit_frag(udp,
                      s,
                      msg->request_id,
                      (uint32_t)env_len,
                      (uint32_t)off,
                      (uint32_t)chunk,
                      &view,
                      last,
                      now,
                      msgs,
                      &nm);
        } else {
            nb_udp_txq_t *q = (nb_udp_txq_t *)calloc(1, sizeof(*q));
            if (!q) {
                break;
            }
            q->buf = (uint8_t *)malloc(env_len);
            if (!q->buf) {
                free(q);
                break;
            }
            nb_env_view_copy(&view, 0, env_len, q->buf);
            q->sent = (uint32_t)off;
            q->total = (uint32_t)env_len;
            q->msgid = msg->request_id;
            if (s->txq_tail) {
                s->txq_tail->next = q;
            } else {
                s->txq_head = q;
            }
            s->txq_tail = q;
            s->txq_pending_bytes += (uint32_t)(env_len - off);
            break;
        }
        off += chunk;
    }
    if (nm > 0) {
        nb_udp_send_msgs(s, msgs, nm);
    }
    if (nfrag >= 2 && s->txq_head == NULL && nb_udp_fec_enabled()) {
        nb_send_fec(udp, s, &view, env_len, msg->request_id);
    }
    return NB_OK;
}

static void nb_session_txq_drain_budget(nb_udp_transport_t *udp,
                                     nb_session_t *s,
                                     uint64_t now,
                                     uint32_t budget)
{
    ntk_udp_dgram_t msgs[TX_BATCH_MAX];
    unsigned nm = 0;
    nb_env_view_t view;

    while (s->txq_head && budget > 0) {
        nb_udp_txq_t *q = s->txq_head;
        /* 排空时 txq 已是连续逻辑缓冲：整段作为 prefix，无独立 payload。 */
        view.prefix = q->buf;
        view.prefix_len = q->total;
        view.payload = NULL;
        view.payload_len = 0;
        while (q->sent < q->total && budget > 0) {
            uint32_t off = q->sent;
            uint32_t chunk = q->total - q->sent;
            int last;
            if (chunk > udp->base.frag_bytes) {
                chunk = udp->base.frag_bytes;
            }
            last = (off + chunk) >= q->total;
            if (nm == TX_BATCH_MAX) {
                nb_udp_send_msgs(s, msgs, nm);
                nm = 0;
            }
            nb_emit_frag(udp, s, q->msgid, q->total, off, chunk, &view, last, now, msgs, &nm);
            q->sent += chunk;
            budget--;
            if (s->txq_pending_bytes >= chunk) {
                s->txq_pending_bytes -= chunk;
            } else {
                s->txq_pending_bytes = 0;
            }
        }
        if (q->sent >= q->total) {
            s->txq_head = q->next;
            if (!s->txq_head) {
                s->txq_tail = NULL;
            }
            free(q->buf);
            free(q);
        }
    }
    if (nm > 0) {
        nb_udp_send_msgs(s, msgs, nm);
    }
}

static void nb_session_txq_drain(nb_udp_transport_t *udp, nb_session_t *s, uint64_t now)
{
    nb_session_txq_drain_budget(udp, s, now, s->cc_burst);
}

/* ---- 接收 ---- */

/* 解码 envelope 并以统一消息上下文上抛给 core。 */
static void nb_deliver_message(nb_udp_transport_t *udp,
                            nb_session_t *s,
                            const uint8_t *buf,
                            size_t len)
{
    nb_envelope_t env;
    nb_message_t msg;
    char route[MAX_ROUTE];

    if (nb_envelope_decode(buf, len, &env) != NB_OK) {
        return;
    }
    nb_envelope_to_message(&env, route, sizeof(route), &msg);
    msg.peer_ip = s->peer_ip_s;
    msg.peer_port = s->peer_port;
    msg.peer_token = s;
    nb_node_on_message(udp->base.node, &udp->base, &msg);
}

static int nb_session_on_data(nb_udp_transport_t *udp,
                           nb_session_t *s,
                           const nb_frag_t *v,
                           uint64_t now_ms)
{
    nb_reasm_slot_t *r;
    uint32_t frag_count;
    uint32_t fidx;

    if (!s || !v) {
        return NB_ERR_INVALID;
    }
    if (v->pt == PT_FEC) {
        nb_fec_on_parity(udp, s, v, now_ms);
        return NB_OK;
    }
    s->rx_pkts++;
    /* Peer data arrived: everything we sent so far was received, and we are
     * no longer idling, so reset the sender-side RTO state. */
    s->last_rx_data_ms = now_ms;
    s->acked_upto = s->send_seq;
    s->rto_attempts = 0;
    NB_METRIC_INC(udp->base.metrics->udp_packets_received);
    if (!s->ssrc_remote_known) {
        s->ssrc_remote = v->ssrc;
        s->ssrc_remote_known = 1;
    }

    if (!nb_rx_window_accept(&s->rx_window, v->seq)) {
        return NB_OK;
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
                nb_atomic_store_u32(&s->metrics->rtcp_jitter_ms, (uint32_t)s->jitter);
            }
        }
        s->transit_prev = transit;
        s->have_transit = 1;
    }

    if (v->total_len > udp->base.max_msg_bytes + MAX_ENVELOPE_EXTRA) {
        return NB_ERR_TOOBIG;
    }
    if ((v->frag_off % udp->base.frag_bytes) != 0) {
        return NB_ERR_INVALID;
    }
    frag_count =
        v->total_len ? (v->total_len + udp->base.frag_bytes - 1) / udp->base.frag_bytes : 1;
    fidx = v->frag_off / udp->base.frag_bytes;
    if (fidx >= frag_count || (uint32_t)v->frag_off + v->frag_len > v->total_len) {
        return NB_ERR_INVALID;
    }

    /* 单分片快路径：解码完整 envelope 后投递。 */
    if (frag_count == 1) {
        nb_deliver_message(udp, s, v->chunk, v->frag_len);
        return NB_OK;
    }

    r = nb_reasm_acquire(&s->reasm, v->msg_id, v->total_len, frag_count, now_ms);
    if (!r) {
        return NB_ERR_NOMEM;
    }
    if (!r->base_seq_set) {
        r->base_seq = (uint16_t)(v->seq - fidx);
        r->base_seq_set = 1;
    }
    (void)nb_reasm_put(r, fidx, v->frag_off, v->chunk, v->frag_len);

    if (r->got == r->frag_count) {
        nb_deliver_message(udp, s, r->buf, r->total_len);
        nb_reasm_release(&s->reasm, r);
        return NB_OK;
    }
    /* FEC：若本消息恰好缺一枚分片且已缓存其校验包，尝试重建补齐。 */
    nb_fec_try_reconstruct(udp, s, v->msg_id, now_ms);
    return NB_OK;
}

static void nb_session_on_nack(nb_udp_transport_t *udp,
                            nb_session_t *s,
                            const uint16_t *seqs,
                            size_t n)
{
    size_t i;
    int lost_sent = 0;
    int unsent = 0;
    if (!s || !seqs) {
        return;
    }
    (void)udp;
    for (i = 0; i < n; i++) {
        uint32_t len = 0;
        const uint8_t *packet;
        /*
         * seq >= send_seq 表示该分片还在 txq 里尚未发出；接收端看不到它才报
         * NACK。这不是丢包，不能据此减窗，否则会形成“越限速越 NACK、越 NACK
         * 越限速”的自激振荡（16 KiB 消息会从亚毫秒退化到几十毫秒）。
         */
        if ((int16_t)(seqs[i] - s->send_seq) >= 0) {
            unsent = 1;
            continue;
        }
        packet = nb_tx_ring_get(&s->tx_ring, seqs[i], &len);
        if (packet) {
            (void)ntk_socket_sendto(s->sock, packet, len, s->peer_ip_s, s->peer_port);
            s->retx++;
            lost_sent = 1;
            if (s->metrics) {
                NB_METRIC_INC(s->metrics->udp_retransmissions);
            }
        }
    }
    /*
     * 对端在等尚未发出的分片：立即按批量上限排空 txq，而不是等下一个 tick。
     * 注意不能用已崩到 1 的 cc_burst 作为预算，否则每次 NACK 只能多发一片。
     */
    if (unsent) {
        nb_session_txq_drain_budget(udp, s, nb_now_ms(), TX_BATCH_MAX);
    }
    /* 只有确实重传了已发送分片才视为拥塞丢包并减窗。 */
    if (lost_sent) {
        uint64_t now = nb_now_ms();
        if (now - s->cc_last_loss_ms > 20) {
            if (s->cc_burst > CC_BURST_MIN) {
                s->cc_burst = s->cc_burst / 2;
            }
            s->cc_last_loss_ms = now;
        }
    }
}

/* ---- RTCP 报告（RFC 3550 SR/RR） ---- */

static uint32_t ntp_msw(uint64_t wall_ms)
{
    return (uint32_t)(wall_ms / 1000ull + 2208988800ull); /* 自 1900 起的秒 */
}

static uint32_t ntp_lsw(uint64_t wall_ms)
{
    return (uint32_t)(((wall_ms % 1000ull) * 4294967296ull) / 1000ull);
}

/* NTP 时间戳的中间 32 位：秒低 16 位 + 小数高 16 位（用于 LSR/DLSR）。 */
static uint32_t nb_ntp_middle32(uint32_t msw, uint32_t lsw)
{
    return (uint32_t)(((msw & 0xffffu) << 16) | (lsw >> 16));
}

static void nb_fill_report_block(const nb_session_t *s, nb_rtcp_report_block_t *rb, uint64_t now_ms)
{
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
    rb->dlsr =
        s->have_last_rx_sr ? (uint32_t)(((now_ms - s->last_rx_sr_ms) * 65536ull) / 1000ull) : 0u;
}

/* 从对端报告块中提取关于本端流（ssrc_local）的丢包/抖动/RTT。 */
static void nb_apply_report_blocks(nb_session_t *s, const nb_rtcp_report_block_t *rb, int n)
{
    uint32_t now32 = nb_ntp_middle32(ntp_msw(ntk_wall_ms()), ntp_lsw(ntk_wall_ms()));
    int i;
    for (i = 0; i < n; i++) {
        if (rb[i].ssrc != s->ssrc_local) {
            continue;
        }
        s->rtcp_remote_lost = rb[i].cumulative_lost;
        if (s->metrics) {
        nb_atomic_store_u32((uint32_t *)&s->metrics->rtcp_remote_lost,
                             (uint32_t)rb[i].cumulative_lost);
        }
        if (rb[i].lsr != 0) {
            /* RTT = now - LSR - DLSR，单位 1/65536 秒。 */
            uint32_t d = now32 - rb[i].lsr - rb[i].dlsr;
            uint32_t rtt_ms = (uint32_t)(((uint64_t)d * 1000ull) >> 16);
            s->last_rtt_ms = rtt_ms;
            if (s->metrics) {
                nb_atomic_store_u32(&s->metrics->rtcp_rtt_ms, rtt_ms);
            }
        }
    }
}

static void nb_session_on_rtcp(
    nb_udp_transport_t *udp, nb_session_t *s, const uint8_t *buf, size_t n, uint64_t now_ms)
{
    (void)udp;
    if (!s || !buf || n < 4) {
        return;
    }
    if (buf[1] == RTCP_SR) {
        nb_rtcp_sr_t sr;
        if (nb_frame_parse_sr(buf, n, &sr) == 1) {
            s->rtcp_sr_received++;
            if (s->metrics) {
                NB_METRIC_INC(s->metrics->rtcp_sr_received);
            }
            s->ssrc_remote = sr.ssrc;
            s->ssrc_remote_known = 1;
            s->last_rx_sr_ntp = nb_ntp_middle32(sr.ntp_msw, sr.ntp_lsw);
            s->last_rx_sr_ms = now_ms;
            s->have_last_rx_sr = 1;
            nb_apply_report_blocks(s, sr.rb, sr.rb_count);
        }
    } else if (buf[1] == RTCP_RR) {
        nb_rtcp_rr_t rr;
        if (nb_frame_parse_rr(buf, n, &rr) == 1) {
            s->rtcp_rr_received++;
            if (s->metrics) {
                NB_METRIC_INC(s->metrics->rtcp_rr_received);
            }
            if (!s->ssrc_remote_known) {
                s->ssrc_remote = rr.ssrc;
                s->ssrc_remote_known = 1;
            }
            nb_apply_report_blocks(s, rr.rb, rr.rb_count);
        }
    }
}

/* 周期发送复合 RTCP 包：本端有发送记录则 SR，否则 RR；尾部附 SDES CNAME。 */
static void nb_session_send_rtcp(nb_udp_transport_t *udp, nb_session_t *s, uint64_t now_ms)
{
    uint8_t buf[256];
    nb_rtcp_report_block_t rb;
    int have_rx = s->ssrc_remote_known && s->rx_pkts > 0;
    size_t n = 0;
    size_t total;
    (void)udp;

    memset(&rb, 0, sizeof(rb));
    if (have_rx) {
        nb_fill_report_block(s, &rb, now_ms);
    }
    if (s->rtcp_pkt_count > 0) {
        uint32_t msw = ntp_msw(s->rtcp_last_rtp_wall);
        uint32_t lsw = ntp_lsw(s->rtcp_last_rtp_wall);
        n = nb_frame_build_sr(buf,
                               sizeof(buf),
                               s->ssrc_local,
                               msw,
                               lsw,
                               s->rtcp_last_rtp_ts,
                               s->rtcp_pkt_count,
                               s->rtcp_octet_count,
                               have_rx ? &rb : NULL,
                               have_rx ? 1 : 0);
        if (n > 0) {
            s->rtcp_last_sr_ntp = nb_ntp_middle32(msw, lsw);
            s->rtcp_sr_sent++;
            if (s->metrics) {
                NB_METRIC_INC(s->metrics->rtcp_sr_sent);
            }
        }
    } else {
        n = nb_frame_build_rr(
            buf, sizeof(buf), s->ssrc_local, have_rx ? &rb : NULL, have_rx ? 1 : 0);
        if (n > 0) {
            s->rtcp_rr_sent++;
            if (s->metrics) {
                NB_METRIC_INC(s->metrics->rtcp_rr_sent);
            }
        }
    }
    if (n == 0) {
        return;
    }
    total = n + nb_frame_build_sdes_cname(buf + n, sizeof(buf) - n, s->ssrc_local, "rpc");
    (void)ntk_socket_sendto(s->sock, buf, total, s->peer_ip_s, s->peer_port);
    s->rx_expected_prior = s->rx_max_seq_set ? (s->rx_cycles + s->rx_max_seq + 1u) : 0u;
    s->rx_received_prior = s->rx_received;
}

/* 会话结束时尽力发送 RTCP BYE，通知对端本端要离开。 */
static void nb_session_send_bye(nb_udp_transport_t *udp, nb_session_t *s)
{
    uint8_t buf[16];
    size_t n;
    (void)udp;
    if (!s || !s->sock) {
        return;
    }
    n = nb_frame_build_bye(buf, sizeof(buf), s->ssrc_local);
    if (n == 0) {
        return;
    }
    (void)ntk_socket_sendto(s->sock, buf, n, s->peer_ip_s, s->peer_port);
    if (s->metrics) {
        NB_METRIC_INC(s->metrics->rtcp_bye_sent);
    }
}

static void nb_reasm_on_lost(const nb_reasm_slot_t *slot, void *user)
{
    nb_session_t *s = (nb_session_t *)user;
    (void)slot;
    if (s) {
        s->lost++;
        if (s->metrics) {
            NB_METRIC_INC(s->metrics->udp_reassembly_expired);
        }
    }
}

/* Sender-side per-message RTO (Plan A): when the session has been idle (no peer
 * data) for >= RTO, retransmit the unacked tail. This recovers a lost last
 * fragment that the peer rx_window cannot flag (no higher seq => no NACK).
 * RTO is overridable via env RTO_MS (ms), default 100. */
#define RTO_MAX_ROUNDS 6

static uint32_t nb_udp_rto_env_ms(void)
{
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
static uint32_t nb_session_rto_ms(const nb_session_t *s)
{
    uint32_t env = nb_udp_rto_env_ms();
    (void)s;
    return env > 0 ? env : 10u;
}

static void nb_session_tick(nb_udp_transport_t *udp, nb_session_t *s, uint64_t now_ms)
{
    uint16_t missing[MAX_NACK_SEQS];
    size_t mn = 0;

    if (!s || !s->used) {
        return;
    }

    nb_session_txq_drain(udp, s, now_ms);

    /* Plan A: idle sender-side RTO retransmit of the unacked tail. */
    if (s->tx_pkts > 0 && !s->txq_head && s->rto_attempts < RTO_MAX_ROUNDS &&
        (now_ms - s->last_rx_data_ms) >= nb_session_rto_ms(s) &&
        (int16_t)(s->send_seq - s->acked_upto) > 0) {
        uint32_t rto = nb_session_rto_ms(s);
        unsigned i;
        for (i = 0; i < TX_BATCH_MAX; i++) {
            uint16_t seq = (uint16_t)(s->send_seq - 1u - i);
            const uint8_t *pkt;
            uint64_t sent;
            uint32_t len = 0;
            if ((int16_t)(seq - s->acked_upto) < 0) {
                break;
            }
            sent = nb_tx_ring_sent_ms(&s->tx_ring, seq);
            if (sent == 0 || now_ms - sent < rto) {
                continue;
            }
            pkt = nb_tx_ring_get(&s->tx_ring, seq, &len);
            if (!pkt || len == 0) {
                continue;
            }
            (void)ntk_socket_sendto(s->sock, pkt, len, s->peer_ip_s, s->peer_port);
            nb_tx_ring_mark_sent(&s->tx_ring, seq, now_ms);
            s->retx++;
            if (s->metrics) {
                NB_METRIC_INC(s->metrics->udp_retransmissions);
            }
        }
        s->rto_attempts++;
    }

    if (s->cc_burst < s->cc_burst_max && now_ms - s->cc_last_loss_ms > 200 &&
        now_ms - s->cc_last_grow_ms > 50) {
        s->cc_burst++;
        s->cc_last_grow_ms = now_ms;
    }

    mn += nb_rx_window_missing(&s->rx_window, missing + mn, MAX_NACK_SEQS - mn);
    mn += nb_reasm_missing(&s->reasm, missing + mn, MAX_NACK_SEQS - mn);

    if (mn > 0) {
        if (s->first_gap_ms == 0) {
            s->first_gap_ms = now_ms;
        }
        if (now_ms - s->last_nack_ms >= NACK_INTERVAL_MS) {
            uint8_t nack[256];
            size_t n = nb_frame_build_nack(
                nack, sizeof(nack), s->ssrc_local, s->ssrc_remote, missing, mn);
            if (n > 0) {
                (void)ntk_socket_sendto(s->sock, nack, n, s->peer_ip_s, s->peer_port);
                s->last_nack_ms = now_ms;
                if (s->metrics) {
                    NB_METRIC_INC(s->metrics->udp_nacks_sent);
                }
            }
        }
    } else {
        s->first_gap_ms = 0;
    }

    if (!udp->base.rtcp_disabled && (s->rx_pkts > 0 || s->tx_pkts > 0) &&
        now_ms - s->rtcp_last_send_ms >=
            (udp->base.rtcp_interval_ms ? udp->base.rtcp_interval_ms : RTCP_INTERVAL_MS)) {
        nb_session_send_rtcp(udp, s, now_ms);
        s->rtcp_last_send_ms = now_ms;
    }

    nb_reasm_expire(&s->reasm, now_ms, REASM_TIMEOUT_MS, nb_reasm_on_lost, s);
}

/* ---- 数据面接收 ---- */

static void nb_data_recv_cb(ntk_socket *sock, void *user)
{
    nb_udp_io_ctx_t *ctx = (nb_udp_io_ctx_t *)user;
    nb_udp_transport_t *udp = ctx ? ctx->udp : NULL;
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
            uint64_t now = nb_now_ms();
            for (i = 0; i < (unsigned)got; i++) {
                uint8_t *buf = (uint8_t *)recvs[i].data;
                size_t n = recvs[i].len;
                if (n < 2) {
                    continue;
                }
                if (buf[1] == RTCP_SR || buf[1] == RTCP_RR) {
                    nb_session_t *s = nb_session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                    if (s) {
                        nb_session_on_rtcp(udp, s, buf, n, now);
                    }
                    continue;
                }
                if (buf[1] == RTCP_BYE) {
                    nb_session_t *s = nb_session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                    if (s) {
                        s->ssrc_remote_known = 0;
                        s->have_last_rx_sr = 0;
                        if (s->metrics) {
                            NB_METRIC_INC(s->metrics->rtcp_bye_received);
                        }
                    }
                    continue;
                }
                if (buf[1] == RTCP_RTPFB) {
                    uint16_t seqs[MAX_NACK_SEQS];
                    int c = nb_frame_parse_nack(buf, n, NULL, NULL, seqs, MAX_NACK_SEQS);
                    if (c > 0) {
                        nb_session_t *s =
                            nb_session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                        if (s) {
                            nb_session_on_nack(udp, s, seqs, (size_t)c);
                        }
                    }
                    continue;
                }
                {
                    nb_frag_t v;
                    int r = nb_frame_parse_data(buf, n, &v);
                    if (r == 1) {
                        nb_session_t *s =
                            nb_session_for(udp, io, sock, recvs[i].ip, recvs[i].port, now);
                        if (s) {
                            (void)nb_session_on_data(udp, s, &v, now);
                        }
                    }
                }
            }
        }
    }
}

/* ---- 定时器 ---- */

static void nb_udp_tick_cb(void *user)
{
    nb_udp_io_ctx_t *ctx = (nb_udp_io_ctx_t *)user;
    nb_udp_transport_t *udp = ctx->udp;
    uint64_t now = nb_now_ms();
    int i;
    for (i = 0; i < PEERS_PER_IO; i++) {
        nb_session_tick(udp, &udp->sessions[ctx->io][i], now);
    }
}

/* 当前 poller 对应的 io 分片下标；找不到回退 0。 */
static int nb_udp_io_current(nb_udp_transport_t *udp)
{
    ntk_poller *cur = ntk_poller_current();
    int i;
    if (cur) {
        for (i = 0; i < udp->io_count; i++) {
            if (udp->pollers[i] == cur) {
                return i;
            }
        }
    }
    return 0;
}

/* ---- 传输接口 ---- */

static int nb_udp_start(nb_transport_t *t)
{
    nb_udp_transport_t *udp = (nb_udp_transport_t *)t;
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
        cbs.on_readable = nb_data_recv_cb;
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
        return NB_ERR_IO;
    }
    for (i = 0; i < udp->io_count; i++) {
        udp->tick_timers[i] = ntk_timer_start(
            udp->pollers[i], nb_tick_interval_ms(), 1, nb_udp_tick_cb, &udp->io_ctx[i]);
    }
    return NB_OK;
}

static void nb_udp_stop(nb_transport_t *t)
{
    nb_udp_transport_t *udp = (nb_udp_transport_t *)t;
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
                nb_session_send_bye(udp, &udp->sessions[i][j]);
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
                nb_session_cleanup(&udp->sessions[i][j]);
                udp->sessions[i][j].used = 0;
            }
        }
    }
}

static int nb_udp_send(nb_transport_t *t, const nb_message_t *msg)
{
    nb_udp_transport_t *udp = (nb_udp_transport_t *)t;
    nb_session_t *s;
    int io;
    if (!msg || !msg->peer_ip || !msg->peer_port || udp->io_count == 0) {
        return NB_ERR_INVALID;
    }
    io = nb_udp_io_current(udp);
    s = nb_session_for(udp, io, udp->socks[io], msg->peer_ip, msg->peer_port, nb_now_ms());
    if (!s) {
        return NB_ERR_NOMEM;
    }
    return nb_session_send(udp, s, msg);
}

static int nb_udp_reply(nb_transport_t *t, const nb_message_t *msg)
{
    nb_udp_transport_t *udp = (nb_udp_transport_t *)t;
    nb_session_t *s;
    if (!msg) {
        return NB_ERR_INVALID;
    }
    s = (nb_session_t *)msg->peer_token;
    if (!s || !s->used) {
        int io;
        if (!msg->peer_ip || !msg->peer_port || udp->io_count == 0) {
            return NB_ERR_INVALID;
        }
        io = nb_udp_io_current(udp);
        s = nb_session_for(udp, io, udp->socks[io], msg->peer_ip, msg->peer_port, nb_now_ms());
        if (!s) {
            return NB_ERR_NOMEM;
        }
    }
    return nb_session_send(udp, s, msg);
}

static int nb_udp_peer_stats(nb_transport_t *t, const char *ip, uint16_t port, nb_peer_stats_t *out)
{
    nb_udp_transport_t *udp = (nb_udp_transport_t *)t;
    int io;
    int i;
    if (!udp || !ip || !out) {
        return -1;
    }
    for (io = 0; io < udp->io_count; io++) {
        for (i = 0; i < PEERS_PER_IO; i++) {
            nb_session_t *s = &udp->sessions[io][i];
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

static const nb_transport_ops_t nb_g_udp_ops = {
    nb_udp_start, nb_udp_stop, nb_udp_send, nb_udp_reply, nb_udp_peer_stats,
};

nb_transport_t *nb_udp_transport_create(void)
{
    nb_udp_transport_t *udp = (nb_udp_transport_t *)calloc(1, sizeof(*udp));
    if (!udp) {
        return NULL;
    }
    udp->base.ops = &nb_g_udp_ops;
    udp->base.kind = NB_TRANSPORT_UDP;
    nb_copy_str(udp->base.scheme, sizeof(udp->base.scheme), "udp", "udp");
    return &udp->base;
}
