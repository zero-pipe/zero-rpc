#include "tx_ring.h"

#include <stdlib.h>

int nb_tx_ring_ensure(nb_tx_ring_t *ring, uint32_t stride)
{
    if (!ring || stride == 0) {
        return 0;
    }
    if (ring->arena && ring->stride >= stride) {
        return 1;
    }
    {
        uint8_t *arena = (uint8_t *)realloc(ring->arena, (size_t)RTX_RING * stride);
        if (!arena) {
            return 0;
        }
        ring->arena = arena;
        ring->stride = stride;
    }
    return 1;
}

uint8_t *nb_tx_ring_slot(nb_tx_ring_t *ring, uint16_t seq)
{
    uint32_t index;
    if (!ring || !ring->arena || ring->stride == 0) {
        return NULL;
    }
    index = (uint32_t)(seq % RTX_RING);
    return ring->arena + (size_t)index * ring->stride;
}

void nb_tx_ring_commit(nb_tx_ring_t *ring, uint16_t seq, uint32_t len)
{
    uint32_t index;
    nb_tx_slot_t *slot;
    if (!ring || !ring->arena || len == 0 || len > ring->stride) {
        return;
    }
    index = (uint32_t)(seq % RTX_RING);
    slot = &ring->slots[index];
    slot->used = 1;
    slot->seq = seq;
    slot->len = len;
    slot->off = index * ring->stride;
    slot->sent_ms = 0;
}

const uint8_t *nb_tx_ring_get(const nb_tx_ring_t *ring, uint16_t seq, uint32_t *out_len)
{
    const nb_tx_slot_t *slot;
    uint32_t index;
    if (!ring || !ring->arena) {
        return NULL;
    }
    index = (uint32_t)(seq % RTX_RING);
    slot = &ring->slots[index];
    if (!slot->used || slot->seq != seq) {
        return NULL;
    }
    if (out_len) {
        *out_len = slot->len;
    }
    return ring->arena + slot->off;
}

void nb_tx_ring_mark_sent(nb_tx_ring_t *ring, uint16_t seq, uint64_t ms)
{
    uint32_t index;
    nb_tx_slot_t *slot;
    if (!ring || !ring->arena) {
        return;
    }
    index = (uint32_t)(seq % RTX_RING);
    slot = &ring->slots[index];
    if (slot->used && slot->seq == seq) {
        slot->sent_ms = ms;
    }
}

uint64_t nb_tx_ring_sent_ms(const nb_tx_ring_t *ring, uint16_t seq)
{
    uint32_t index;
    const nb_tx_slot_t *slot;
    if (!ring || !ring->arena) {
        return 0;
    }
    index = (uint32_t)(seq % RTX_RING);
    slot = &ring->slots[index];
    if (slot->used && slot->seq == seq) {
        return slot->sent_ms;
    }
    return 0;
}

void nb_tx_ring_cleanup(nb_tx_ring_t *ring)
{
    if (!ring) {
        return;
    }
    free(ring->arena);
    ring->arena = NULL;
    ring->stride = 0;
    for (size_t i = 0; i < RTX_RING; i++) {
        ring->slots[i].used = 0;
    }
}
