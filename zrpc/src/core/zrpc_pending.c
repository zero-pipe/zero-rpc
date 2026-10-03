/* pending：客户端未完成调用的关联表（request_id -> 回调/超时）。
 * 仅在节点 poller 线程上访问；超时由 node 定时器触发。
 */
#include "zrpc_internal.h"

#include <stdlib.h>

void zrpc_pending_invoke(zrpc_pending_t *p, int status, const zrpc_response_t *resp) {
    zrpc_response_fn cb;
    void *user;
    zrpc_node_t *node;
    if (!p) {
        return;
    }
    cb = p->cb;
    user = p->user;
    node = p->node;
    p->used = 0;
    p->node = NULL;
    if (p->data) {
        zrpc_mem_free(p->data);
        p->data = NULL;
    }
    p->has_req = 0;
    p->len = 0;
    if (node) {
        if (status == ZRPC_ERR_TIMEOUT) {
            node->metrics.timeouts++;
        }
        if (status != ZRPC_OK) {
            node->metrics.requests_failed++;
        }
    }
    if (p->streaming) {
        zrpc_stream_done_fn done = p->done_cb;
        void *stream_user = p->stream_user;
        p->chunk_cb = NULL;
        p->done_cb = NULL;
        if (done) {
            done(status, stream_user);
        }
    } else if (cb) {
        cb(status, resp, user);
    }
}

void zrpc_pending_release(zrpc_pending_t *p) {
    if (!p) {
        return;
    }
    p->used = 0;
    if (p->data) {
        zrpc_mem_free(p->data);
        p->data = NULL;
    }
    p->has_req = 0;
    p->len = 0;
    p->streaming = 0;
    p->chunk_cb = NULL;
    p->done_cb = NULL;
    p->stream_user = NULL;
}

zrpc_pending_t *zrpc_pending_add(zrpc_node_t *node, uint64_t id, zrpc_response_fn cb, void *user,
                                 uint64_t timeout_ms) {
    int i;
    for (i = 0; i < ZRPC_PENDING_MAX; i++) {
        if (!node->pending[i].used) {
            zrpc_pending_t *p = &node->pending[i];
            p->used = 1;
            p->node = node;
            p->request_id = id;
            p->cb = cb;
            p->user = user;
            p->deadline_ms = zrpc_now_ms() + (timeout_ms ? timeout_ms : ZRPC_DEFAULT_TIMEOUT_MS);
            return p;
        }
    }
    return NULL;
}

zrpc_pending_t *zrpc_pending_find(zrpc_node_t *node, uint64_t id) {
    int i;
    for (i = 0; i < ZRPC_PENDING_MAX; i++) {
        if (node->pending[i].used && node->pending[i].request_id == id) {
            return &node->pending[i];
        }
    }
    return NULL;
}

void zrpc_pending_tick(zrpc_node_t *node, uint64_t now_ms) {
    int i;
    for (i = 0; i < ZRPC_PENDING_MAX; i++) {
        zrpc_pending_t *p = &node->pending[i];
        if (!p->used) {
            continue;
        }
        if (now_ms >= p->deadline_ms) {
            zrpc_pending_invoke(p, ZRPC_ERR_TIMEOUT, NULL);
            continue;
        }
        /* RTO 超时重传请求（覆盖整包丢失：NACK 无从触发的情况）。
         * 仅用于 UDP：TCP 本身可靠有序，应用层重传只会产生重复请求。 */
        if (p->has_req && p->transport == ZRPC_TRANSPORT_UDP &&
            p->retries < ZRPC_MAX_RETX && now_ms >= p->next_retx_ms) {
            uint32_t shift = p->retries < 4 ? p->retries : 4;
            zrpc_pending_retx(node, p);
            p->retries++;
            p->next_retx_ms = now_ms + (uint64_t)ZRPC_RTO_INIT_MS * ((uint64_t)1u << shift);
        }
    }
}
