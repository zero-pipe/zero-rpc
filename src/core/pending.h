#ifndef PENDING_H
#define PENDING_H

/* pending：客户端未完成调用的关联表（request_id -> 回调/超时）。 */

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#include <uthash.h>

#include "caps.h"

typedef struct zri_pending {
    int used;
    node_t *node;
    uint64_t request_id;
    response_fn cb;
    void *user;
    uint64_t deadline_ms;
    /* RTO 重传（弱网整包丢失时 NACK 无从触发，需超时重发请求） */
    int has_req;
    int streaming;
    response_chunk_fn chunk_cb;
    stream_done_fn done_cb;
    void *stream_user;
    uint64_t expected_offset;
    uint32_t bp_retries;
    uint64_t next_retx_ms;
    uint32_t retries;
    char service[SERVICE_NAME_MAX];
    char method[METHOD_NAME_MAX];
    char ip[ENDPOINT_MAX];
    uint16_t port;
    transport_kind_t transport;
    char scheme[SCHEME_MAX];
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    UT_hash_handle hh;              /* request_id 哈希索引 */
    struct zri_pending *next;      /* 节点持有的全部槽位链表 */
    struct zri_pending *free_next; /* 空闲槽位复用链表 */
} zri_pending_t;

zri_pending_t *zri_pending_add(node_t *node, uint64_t id, response_fn cb, void *user,
                                 uint64_t timeout_ms);
zri_pending_t *zri_pending_find(node_t *node, uint64_t id);
void zri_pending_invoke(zri_pending_t *p, int status, const response_t *resp);
void zri_pending_release(zri_pending_t *p);
void zri_pending_tick(node_t *node, uint64_t now_ms);
void zri_pending_cleanup(node_t *node);

/* RTO 重传：按保存的请求参数重发（实现于 proxy.c）。 */
void zri_pending_retx(node_t *node, zri_pending_t *p);

#endif /* PENDING_H */
