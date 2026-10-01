#include "zrpc_tx_ring.h"

#include <stdlib.h>

int zrpc_tx_ring_ensure(zrpc_tx_ring_t *ring, uint32_t stride) {
    if (!ring || stride == 0) {
        return 0;
    }
    if (ring->arena && ring->stride >= stride) {
        return 1;
    }
    {
        uint8_t *arena = (uint8_t *)realloc(ring->arena, (size_t)ZRPC_RTX_RING * stride);
        if (!arena) {
            return 0;
        }
        ring->arena = arena;
        ring->stride = stride;
    }
    return 1;
}

uint8_t *zrpc_tx_ring_slot(zrpc_tx_ring_t *ring, uint16_t seq) {
    uint32_t index;
    if (!ring || !ring->arena || ring->stride == 0) {
        return NULL;
    }
    index = (uint32_t)(seq % ZRPC_RTX_RING);
    return ring->arena + (size_t)index * ring->stride;
}

void zrpc_tx_ring_commit(zrpc_tx_ring_t *ring, uint16_t seq, uint32_t len) {
    uint32_t index;
    zrpc_tx_slot_t *slot;
    if (!ring || !ring->arena || len == 0 || len > ring->stride) {
        return;
    }
    index = (uint32_t)(seq % ZRPC_RTX_RING);
    slot = &ring->slots[index];
    slot->used = 1;
    slot->seq = seq;
    slot->len = len;
    slot->off = index * ring->stride;
}

const uint8_t *zrpc_tx_ring_get(const zrpc_tx_ring_t *ring, uint16_t seq, uint32_t *out_len) {
    const zrpc_tx_slot_t *slot;
    uint32_t index;
    if (!ring || !ring->arena) {
        return NULL;
    }
    index = (uint32_t)(seq % ZRPC_RTX_RING);
    slot = &ring->slots[index];
    if (!slot->used || slot->seq != seq) {
        return NULL;
    }
    if (out_len) {
        *out_len = slot->len;
    }
    return ring->arena + slot->off;
}

void zrpc_tx_ring_cleanup(zrpc_tx_ring_t *ring) {
    if (!ring) {
        return;
    }
    free(ring->arena);
    ring->arena = NULL;
    ring->stride = 0;
    for (size_t i = 0; i < ZRPC_RTX_RING; i++) {
        ring->slots[i].used = 0;
    }
}
