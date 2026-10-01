#ifndef ZRPC_ENVELOPE_H
#define ZRPC_ENVELOPE_H

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provider-neutral RPC envelope. All integer fields are big-endian. */
#define ZRPC_ENVELOPE_HDR_LEN 32

/* RPC kind (envelope-level semantics). */
#define ZRPC_KIND_REQUEST 0
#define ZRPC_KIND_RESPONSE 1
#define ZRPC_KIND_EVENT 2

#define ZRPC_STREAM_FLAG_STREAM 0x80
#define ZRPC_STREAM_FLAG_FIRST  ZRPC_STREAM_FIRST
#define ZRPC_STREAM_FLAG_LAST   ZRPC_STREAM_LAST
#define ZRPC_STREAM_FLAG_CANCEL ZRPC_STREAM_CANCEL

typedef struct zrpc_envelope {
    uint8_t kind;
    uint8_t encoding;
    uint8_t stream_flags;
    uint32_t request_id;
    uint32_t stream_id;
    uint64_t stream_offset;
    uint64_t stream_total_size;
    const char *route;
    const void *payload;
    size_t payload_len;
} zrpc_envelope_t;

typedef struct zrpc_envelope_view {
    uint8_t kind;
    uint8_t encoding;
    uint8_t stream_flags;
    uint32_t request_id;
    uint32_t stream_id;
    uint64_t stream_offset;
    uint64_t stream_total_size;
    const char *route;
    uint16_t route_len;
    const uint8_t *payload;
    size_t payload_len;
} zrpc_envelope_view_t;

size_t zrpc_envelope_size(size_t route_len, size_t payload_len);
size_t zrpc_envelope_encode(uint8_t *buf, size_t cap, const zrpc_envelope_t *envelope);
int zrpc_envelope_decode(const uint8_t *buf, size_t len, zrpc_envelope_view_t *view);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_ENVELOPE_H */
