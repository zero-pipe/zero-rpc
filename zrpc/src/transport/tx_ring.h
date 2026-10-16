#ifndef ZRPC_TX_RING_H
#define ZRPC_TX_RING_H

#include <stddef.h>
#include <stdint.h>

#ifndef ZRPC_RTX_RING
#define ZRPC_RTX_RING 128
#endif

/* One retained sent datagram (for NACK-triggered retransmission). */
typedef struct zri_tx_slot {
    int used;
    uint16_t seq;
    uint32_t len;
    uint32_t off;
    uint64_t sent_ms; /* 最近一次发送时刻（0 = 尚未发送），用于发送端 RTO 重传 */
} zri_tx_slot_t;

typedef struct zri_tx_ring {
    uint8_t *arena;
    uint32_t stride;
    zri_tx_slot_t slots[ZRPC_RTX_RING];
} zri_tx_ring_t;

/* Allocate or grow the backing arena. stride is the max datagram size. */
int zri_tx_ring_ensure(zri_tx_ring_t *ring, uint32_t stride);
/* Buffer for building the datagram for seq; caller writes then commits. */
uint8_t *zri_tx_ring_slot(zri_tx_ring_t *ring, uint16_t seq);
void zri_tx_ring_commit(zri_tx_ring_t *ring, uint16_t seq, uint32_t len);
/* Look up a retained datagram by sequence. NULL if evicted. */
const uint8_t *zri_tx_ring_get(const zri_tx_ring_t *ring, uint16_t seq, uint32_t *out_len);
/* Record the last-sent time of a retained datagram (for sender-side RTO). */
void zri_tx_ring_mark_sent(zri_tx_ring_t *ring, uint16_t seq, uint64_t ms);
/* Last-sent time of a retained datagram; 0 if absent or never sent. */
uint64_t zri_tx_ring_sent_ms(const zri_tx_ring_t *ring, uint16_t seq);
void zri_tx_ring_cleanup(zri_tx_ring_t *ring);

#endif /* ZRPC_TX_RING_H */
