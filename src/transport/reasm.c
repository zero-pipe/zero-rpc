#include "reasm.h"
#include "mem.h"

#include <stdlib.h>
#include <string.h>

void nb_reasm_init(nb_reasm_t *reasm)
{
    if (reasm) {
        for (size_t i = 0; i < MAX_REASM; i++) {
            reasm->slots[i].used = 0;
            reasm->slots[i].buf = NULL;
            reasm->slots[i].bits = NULL;
        }
        reasm->budget_bytes = REASM_BUDGET_DEFAULT;
        reasm->used_bytes = 0;
    }
}

void nb_reasm_set_budget(nb_reasm_t *reasm, uint32_t budget_bytes)
{
    if (reasm) {
        reasm->budget_bytes = budget_bytes;
    }
}

void nb_reasm_release(nb_reasm_t *reasm, nb_reasm_slot_t *slot)
{
    if (!slot) {
        return;
    }
    if (reasm && slot->used) {
        if (reasm->used_bytes >= slot->total_len) {
            reasm->used_bytes -= slot->total_len;
        } else {
            reasm->used_bytes = 0;
        }
    }
    if (slot->buf) {
        nb_mem_free(slot->buf);
        slot->buf = NULL;
    }
    if (slot->bits) {
        nb_mem_free(slot->bits);
        slot->bits = NULL;
    }
    slot->used = 0;
}

void nb_reasm_reset(nb_reasm_t *reasm)
{
    if (!reasm) {
        return;
    }
    for (size_t i = 0; i < MAX_REASM; i++) {
        nb_reasm_release(reasm, &reasm->slots[i]);
    }
    reasm->used_bytes = 0;
}

nb_reasm_slot_t *nb_reasm_find(nb_reasm_t *reasm, uint32_t msg_id)
{
    if (!reasm) {
        return NULL;
    }
    for (size_t i = 0; i < MAX_REASM; i++) {
        if (reasm->slots[i].used && reasm->slots[i].msg_id == msg_id) {
            return &reasm->slots[i];
        }
    }
    return NULL;
}

nb_reasm_slot_t *nb_reasm_acquire(
    nb_reasm_t *reasm, uint32_t msg_id, uint32_t total_len, uint32_t frag_count, uint64_t now_ms)
{
    nb_reasm_slot_t *slot = NULL;
    if (!reasm || frag_count == 0 || (total_len > 0 && frag_count == 0)) {
        return NULL;
    }
    for (size_t i = 0; i < MAX_REASM; i++) {
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
    if (reasm->budget_bytes != 0 && reasm->used_bytes + total_len > reasm->budget_bytes) {
        return NULL; /* 超出重组内存预算：丢包由对端超时重传兜底 */
    }
    slot->used = 1;
    slot->msg_id = msg_id;
    slot->total_len = total_len;
    slot->frag_count = frag_count;
    slot->got = 0;
    slot->created_ms = now_ms;
    slot->base_seq_set = 0;
    slot->buf = total_len ? (uint8_t *)nb_mem_alloc(total_len) : NULL;
    /* bitset 走控制对象池，避免每条消息一次 calloc/free。 */
    slot->bits = (uint8_t *)nb_mem_alloc((frag_count + 7) / 8);
    if (slot->bits) {
        memset(slot->bits, 0, (frag_count + 7) / 8);
    }
    if ((total_len && !slot->buf) || !slot->bits) {
        if (slot->buf) {
            nb_mem_free(slot->buf);
            slot->buf = NULL;
        }
        if (slot->bits) {
            nb_mem_free(slot->bits);
            slot->bits = NULL;
        }
        slot->used = 0;
        return NULL;
    }
    reasm->used_bytes += total_len;
    return slot;
}

int nb_reasm_put(
    nb_reasm_slot_t *slot, uint32_t frag_index, uint32_t offset, const uint8_t *data, uint32_t len)
{
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

size_t nb_reasm_missing(const nb_reasm_t *reasm, uint16_t *out, size_t cap)
{
    size_t count = 0;
    if (!reasm || !out || cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < MAX_REASM && count < cap; i++) {
        const nb_reasm_slot_t *slot = &reasm->slots[i];
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

void nb_reasm_expire(nb_reasm_t *reasm,
                      uint64_t now_ms,
                      uint64_t timeout_ms,
                      void (*on_lost)(const nb_reasm_slot_t *slot, void *user),
                      void *user)
{
    if (!reasm) {
        return;
    }
    for (size_t i = 0; i < MAX_REASM; i++) {
        nb_reasm_slot_t *slot = &reasm->slots[i];
        if (slot->used && now_ms - slot->created_ms > timeout_ms) {
            if (on_lost) {
                on_lost(slot, user);
            }
            nb_reasm_release(reasm, slot);
        }
    }
}
