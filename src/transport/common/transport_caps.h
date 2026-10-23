#ifndef TRANSPORT_CAPS_H
#define TRANSPORT_CAPS_H

/* 传输层编译期容量/上限常量（UDP/TCP 扇出、分片、可靠性、RTCP）。 */

#include "framing.h"

#include <stdint.h>
#include <stdlib.h>

/* ---- UDP 分片与背压 ---- */
#define FRAG_DEFAULT 1200 /* UDP 默认每分片 payload 字节 */
#define FRAG_MIN 256
#define FRAG_MAX 8192
#define BACKPRESSURE_DEFAULT (4u * 1024u * 1024u)

/* ---- 传输扇出 ---- */
#define PEERS_PER_IO 64
#define TCP_LINKS_PER_IO 64

/* ---- UDP 可靠性参数 ---- */
#define REASM_TIMEOUT_MS 3000
#define NACK_INTERVAL_MS 10
#define MAX_NACK_SEQS 64
#define RTO_INIT_MS 150
#define MAX_RETX 5
#define TX_BATCH_MAX 32
#define RX_BATCH_MAX 16
/* 拥塞窗口下限：保证 16 KiB（14 个分片）始终能一次批量发出。 */
#define CC_BURST_MIN 16
#define RTX_SLOT_MAX (RTP_HDR_LEN + FRAG_HDR_LEN + FRAG_MAX)

/* ---- RTCP 报告 ---- */
#define RTCP_INTERVAL_MS 1000 /* SR/RR 上报周期 */

#endif /* TRANSPORT_CAPS_H */
