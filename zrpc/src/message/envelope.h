#ifndef ZRPC_ENVELOPE_H
#define ZRPC_ENVELOPE_H

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provider-neutral RPC envelope. All integer fields are big-endian. */
#define ZRPC_ENVELOPE_HDR_LEN 36

/* RPC kind (envelope-level semantics). */
#define ZRPC_KIND_REQUEST 0
#define ZRPC_KIND_RESPONSE 1
#define ZRPC_KIND_EVENT 2

#define ZRPC_STREAM_FLAG_STREAM 0x80
#define ZRPC_STREAM_FLAG_FIRST  ZRPC_STREAM_FIRST
#define ZRPC_STREAM_FLAG_LAST   ZRPC_STREAM_LAST
#define ZRPC_STREAM_FLAG_CANCEL ZRPC_STREAM_CANCEL

/*
 * Single envelope descriptor for both encoding and decoding (no separate
 * "view" type): encode reads the fields, decode fills them. route_len is an
 * output on decode and ignored on encode (derived from strlen(route)).
 *
 * Wire header (big-endian), 36 bytes:
 *   [0]      kind
 *   [1]      encoding
 *   [2]      stream_flags
 *   [3]      reserved
 *   [4..7]   status       (应用状态码；仅 RESPONSE 有意义)
 *   [8..11]  request_id
 *   [12..15] stream_id
 *   [16..23] stream_offset
 *   [24..31] stream_total_size
 *   [32..33] route_len
 *   [34..35] reserved
 */
typedef struct zri_envelope {
    uint8_t kind;
    uint8_t encoding;
    uint8_t stream_flags;
    int status;
    uint32_t request_id;
    uint32_t stream_id;
    uint64_t stream_offset;
    uint64_t stream_total_size;
    const char *route;
    uint16_t route_len; /* decode: filled; encode: ignored */
    const void *payload;
    size_t payload_len;
} zri_envelope_t;

size_t zri_envelope_size(size_t route_len, size_t payload_len);
size_t zri_envelope_encode(uint8_t *buf, size_t cap, const zri_envelope_t *envelope);
int zri_envelope_decode(const uint8_t *buf, size_t len, zri_envelope_t *view);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_ENVELOPE_H */
