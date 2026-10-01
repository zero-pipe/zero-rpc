#include "zrpc_reasm.h"

#include <stdlib.h>
#include <string.h>

void zrpc_reasm_init(zrpc_reasm_t *reasm) {
    if (reasm) {
        for (size_t i = 0; i < ZRPC_MAX_REASM; i++) {
            reasm->slots[i].used = 0;
            reasm->slots[i].buf = NULL;
            reasm->slots[i].bits = NULL;
        }
    }
}

void zrpc_reasm_release(zrpc_reasm_slot_t *slot) {
    if (!slot) {
        return;
    }
    if (slot->buf) {
        free(slot->buf);
        slot->buf = NULL;
    }
    if (slot->bits) {
        free(slot->bits);
        slot->bits = NULL;
    }
    slot->used = 0;
}

void zrpc_reasm_reset(zrpc_reasm_t *reasm) {
    if (!reasm) {
        return;
    }
    for (size_t i = 0; i < ZRPC_MAX_REASM; i++) {
        zrpc_reasm_release(&reasm->slots[i]);
    }
}

zrpc_reasm_slot_t *zrpc_reasm_find(zrpc_reasm_t *reasm, uint32_t msg_id) {
    if (!reasm) {
        return NULL;
    }
    for (size_t i = 0; i < ZRPC_MAX_REASM; i++) {
        if (reasm->slots[i].used && reasm->slots[i].msg_id == msg_id) {
            return &reasm->slots[i];
        }
    }
    return NULL;
}

zrpc_reasm_slot_t *zrpc_reasm_acquire(zrpc_reasm_t *reasm, uint32_t msg_id, uint32_t total_len,
                                      uint32_t frag_count, uint64_t now_ms) {
    zrpc_reasm_slot_t *slot = NULL;
    if (!reasm || frag_count == 0 || (total_len > 0 && frag_count == 0)) {
        return NULL;
    }
    for (size_t i = 0; i < ZRPC_MAX_REASM; i++) {
        if (reasm->slots[i].used && reasm->slots[i].msg_id == msg_id) {
            return &reasm->slots[i];
        }
        if (!reasm->slots[i].used && !slot) {
            slot = &reasm->slots[i];
        }
    }
    if (!slot) {
        return NULL;
    }
    slot->used = 1;
    slot->msg_id = msg_id;
    slot->total_len = total_len;
    slot->frag_count = frag_count;
    slot->got = 0;
    slot->created_ms = now_ms;
    slot->base_seq_set = 0;
    slot->buf = total_len ? (uint8_t *)malloc(total_len) : NULL;
    slot->bits = (uint8_t *)calloc(1, (frag_count + 7) / 8);
    if ((total_len && !slot->buf) || !slot->bits) {
        if (slot->buf) {
            free(slot->buf);
            slot->buf = NULL;
        }
        if (slot->bits) {
            free(slot->bits);
            slot->bits = NULL;
        }
        slot->used = 0;
        return NULL;
    }
    return slot;
}

int zrpc_reasm_put(zrpc_reasm_slot_t *slot, uint32_t frag_index, uint32_t offset,
                   const uint8_t *data, uint32_t len) {
    if (!slot || !slot->bits || frag_index >= slot->frag_count || offset > slot->total_len ||
        len > slot->total_len - offset || (len > 0 && !data)) {
        return 0;
    }
    if (slot->bits[frag_index >> 3] & (uint8_t)(1u << (frag_index & 7))) {
        return 0;
    }
    slot->bits[frag_index >> 3] |= (uint8_t)(1u << (frag_index & 7));
    if (len > 0 && slot->buf && data) {
        memcpy(slot->buf + offset, data, len);
    }
    slot->got++;
    return 1;
}

size_t zrpc_reasm_missing(const zrpc_reasm_t *reasm, uint16_t *out, size_t cap) {
    size_t count = 0;
    if (!reasm || !out || cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < ZRPC_MAX_REASM && count < cap; i++) {
        const zrpc_reasm_slot_t *slot = &reasm->slots[i];
        if (!slot->used || !slot->base_seq_set) {
            continue;
        }
        for (uint32_t index = 0; index < slot->frag_count && count < cap; index++) {
            if (!(slot->bits[index >> 3] & (uint8_t)(1u << (index & 7)))) {
                out[count++] = (uint16_t)(slot->base_seq + index);
            }
        }
    }
    return count;
}

void zrpc_reasm_expire(zrpc_reasm_t *reasm, uint64_t now_ms, uint64_t timeout_ms,
                       void (*on_lost)(const zrpc_reasm_slot_t *slot, void *user), void *user) {
    if (!reasm) {
        return;
    }
    for (size_t i = 0; i < ZRPC_MAX_REASM; i++) {
        zrpc_reasm_slot_t *slot = &reasm->slots[i];
        if (slot->used && now_ms - slot->created_ms > timeout_ms) {
            if (on_lost) {
                on_lost(slot, user);
            }
            zrpc_reasm_release(slot);
        }
    }
}
