#ifndef PENDING_H
#define PENDING_H

/* pending：客户端未完成调用的关联表（request_id -> 回调/超时）。 */

#include <types.h>
#include <ntk.h>

#include <stddef.h>
#include <stdint.h>

#include <uthash.h>

#include "caps.h"

typedef struct nb_pending {
    int used;
    nb_node_t *node;
    ntk_poller *poller; /* 归属 poller：发送/超时/重传/回调都在此线程 */
    struct nb_pending *lane_prev; /* 归属 poller 的 pending lane */
    struct nb_pending *lane_next;
    uint32_t gen; /* 槽位复用代际，tick 迭代时用于识别被复用的槽位 */
    const void *owner; /* 创建该调用的 stub；用于 stub/proxy 销毁时批量取消 */
    uint64_t request_id;
    response_fn cb;
    void *user;
    uint64_t started_ms;  /* 调用发起时刻，用于端到端延迟统计 */
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
    char service[NB_SERVICE_NAME_MAX];
    char method[NB_METHOD_NAME_MAX];
    char ip[NB_ENDPOINT_MAX];
    uint16_t port;
    nb_transport_kind_t transport;
    char scheme[NB_SCHEME_MAX];
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    uint8_t *metadata; /* 请求 metadata 的 TLV 副本，用于重传 */
    uint16_t metadata_len;
    nb_metadata_t *response_metadata; /* 响应 metadata（按需分配） */
    char *response_message;        /* 响应状态消息（按需分配） */
    UT_hash_handle hh;             /* request_id 哈希索引 */
    struct nb_pending *next;      /* 节点持有的全部槽位链表 */
    struct nb_pending *free_next; /* 空闲槽位复用链表 */
} nb_pending_t;

nb_pending_t *nb_pending_add(nb_node_t *node,
                               uint64_t id,
                               response_fn cb,
                               void *user,
                               uint64_t timeout_ms,
                               ntk_poller *poller,
                               int *out_rc);
nb_pending_t *nb_pending_find(nb_node_t *node, uint64_t id);
/* 返回该 request_id 的归属 poller；未找到返回 NULL。 */
ntk_poller *nb_pending_owner(nb_node_t *node, uint64_t id);
void nb_pending_invoke(nb_pending_t *p, int status, const nb_response_t *resp);
void nb_pending_release(nb_pending_t *p);
void nb_pending_tick(nb_node_t *node, uint64_t now_ms, ntk_poller *poller);
void nb_pending_cleanup(nb_node_t *node);

/* RTO 重传：按保存的请求参数重发（实现于 proxy.c）。 */
void nb_pending_retx(nb_node_t *node, nb_pending_t *p);

#endif /* PENDING_H */
