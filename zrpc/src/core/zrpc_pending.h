#ifndef ZRPC_PENDING_H
#define ZRPC_PENDING_H

/* pending：客户端未完成调用的关联表（request_id -> 回调/超时）。 */

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#include "zrpc_caps.h"

typedef struct zrpc_pending {
    int used;
    zrpc_node_t *node;
    uint64_t request_id;
    zrpc_response_fn cb;
    void *user;
    uint64_t deadline_ms;
    /* RTO 重传（弱网整包丢失时 NACK 无从触发，需超时重发请求） */
    int has_req;
    int streaming;
    zrpc_response_chunk_fn chunk_cb;
    zrpc_stream_done_fn done_cb;
    void *stream_user;
    uint64_t expected_offset;
    uint32_t bp_retries;
    uint64_t next_retx_ms;
    uint32_t retries;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method[ZRPC_METHOD_NAME_MAX];
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port;
    zrpc_transport_kind_t transport;
    char scheme[ZRPC_SCHEME_MAX];
    uint8_t *data;
    size_t len;
    uint8_t encoding;
} zrpc_pending_t;

zrpc_pending_t *zrpc_pending_add(zrpc_node_t *node, uint64_t id, zrpc_response_fn cb, void *user,
                                 uint64_t timeout_ms);
zrpc_pending_t *zrpc_pending_find(zrpc_node_t *node, uint64_t id);
void zrpc_pending_invoke(zrpc_pending_t *p, int status, const zrpc_response_t *resp);
void zrpc_pending_release(zrpc_pending_t *p);
void zrpc_pending_tick(zrpc_node_t *node, uint64_t now_ms);

/* RTO 重传：按保存的请求参数重发（实现于 proxy.c）。 */
void zrpc_pending_retx(zrpc_node_t *node, zrpc_pending_t *p);

#endif /* ZRPC_PENDING_H */
