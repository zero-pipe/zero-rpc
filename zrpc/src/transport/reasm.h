#ifndef ZRPC_REASM_H
#define ZRPC_REASM_H

#include <stddef.h>
#include <stdint.h>

#ifndef ZRPC_MAX_REASM
#define ZRPC_MAX_REASM 16
#endif

/* One in-progress message reassembly. Buffers are owned by the slot. */
typedef struct zri_reasm_slot {
    int used;
    uint32_t msg_id;
    uint32_t total_len;
    uint32_t frag_count;
    uint32_t got;
    uint64_t created_ms;
    uint16_t base_seq;
    int base_seq_set;
    uint8_t *buf;
    uint8_t *bits;
} zri_reasm_slot_t;

typedef struct zri_reasm {
    zri_reasm_slot_t slots[ZRPC_MAX_REASM];
} zri_reasm_t;

void zri_reasm_init(zri_reasm_t *reasm);
void zri_reasm_reset(zri_reasm_t *reasm);

/* Find an existing slot or create one for msg_id. NULL on allocation failure. */
zri_reasm_slot_t *zri_reasm_acquire(zri_reasm_t *reasm, uint32_t msg_id, uint32_t total_len,
                                      uint32_t frag_count, uint64_t now_ms);
zri_reasm_slot_t *zri_reasm_find(zri_reasm_t *reasm, uint32_t msg_id);

/* Copy one fragment. Returns 1 when the fragment was new, 0 if duplicate. */
int zri_reasm_put(zri_reasm_slot_t *slot, uint32_t frag_index, uint32_t offset,
                   const uint8_t *data, uint32_t len);

void zri_reasm_release(zri_reasm_slot_t *slot);

/* Collect missing fragments as (base_seq + index) into out. */
size_t zri_reasm_missing(const zri_reasm_t *reasm, uint16_t *out, size_t cap);

/* Release slots older than timeout. on_lost is optional. */
void zri_reasm_expire(zri_reasm_t *reasm, uint64_t now_ms, uint64_t timeout_ms,
                       void (*on_lost)(const zri_reasm_slot_t *slot, void *user), void *user);

#endif /* ZRPC_REASM_H */
