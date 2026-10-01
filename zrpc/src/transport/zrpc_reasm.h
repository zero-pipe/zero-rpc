#ifndef ZRPC_REASM_H
#define ZRPC_REASM_H

#include <stddef.h>
#include <stdint.h>

#ifndef ZRPC_MAX_REASM
#define ZRPC_MAX_REASM 16
#endif

/* One in-progress message reassembly. Buffers are owned by the slot. */
typedef struct zrpc_reasm_slot {
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
} zrpc_reasm_slot_t;

typedef struct zrpc_reasm {
    zrpc_reasm_slot_t slots[ZRPC_MAX_REASM];
} zrpc_reasm_t;

void zrpc_reasm_init(zrpc_reasm_t *reasm);
void zrpc_reasm_reset(zrpc_reasm_t *reasm);

/* Find an existing slot or create one for msg_id. NULL on allocation failure. */
zrpc_reasm_slot_t *zrpc_reasm_acquire(zrpc_reasm_t *reasm, uint32_t msg_id, uint32_t total_len,
                                      uint32_t frag_count, uint64_t now_ms);
zrpc_reasm_slot_t *zrpc_reasm_find(zrpc_reasm_t *reasm, uint32_t msg_id);

/* Copy one fragment. Returns 1 when the fragment was new, 0 if duplicate. */
int zrpc_reasm_put(zrpc_reasm_slot_t *slot, uint32_t frag_index, uint32_t offset,
                   const uint8_t *data, uint32_t len);

void zrpc_reasm_release(zrpc_reasm_slot_t *slot);

/* Collect missing fragments as (base_seq + index) into out. */
size_t zrpc_reasm_missing(const zrpc_reasm_t *reasm, uint16_t *out, size_t cap);

/* Release slots older than timeout. on_lost is optional. */
void zrpc_reasm_expire(zrpc_reasm_t *reasm, uint64_t now_ms, uint64_t timeout_ms,
                       void (*on_lost)(const zrpc_reasm_slot_t *slot, void *user), void *user);

#endif /* ZRPC_REASM_H */
