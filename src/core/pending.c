/* pending：客户端未完成调用的关联表（request_id -> 回调/超时）。
 * 仅在节点 poller 线程上访问；超时由 node 定时器触发。
 */
#include "pending.h"

#include "atomic.h"
#include "mem.h"
#include "node_internal.h"

#include <stdlib.h>

/* 回收一个槽位：清空回调/数据并挂到空闲链表，槽位仍留在节点链表中复用。 */
static void pending_recycle(node_t *node, zri_pending_t *p)
{
    p->used = 0;
    if (node && node->pending_count > 0) {
        node->pending_count--;
    }
    p->node = NULL;
    p->owner = NULL;
    p->cb = NULL;
    p->user = NULL;
    p->streaming = 0;
    p->chunk_cb = NULL;
    p->done_cb = NULL;
    p->stream_user = NULL;
    p->data = NULL;
    p->len = 0;
    p->metadata = NULL;
    p->metadata_len = 0;
    if (p->response_metadata) {
        free(p->response_metadata);
        p->response_metadata = NULL;
    }
    if (p->response_message) {
        free(p->response_message);
        p->response_message = NULL;
    }
    p->has_req = 0;
    p->free_next = node->pending_free;
    node->pending_free = p;
}

void zri_pending_invoke(zri_pending_t *p, int status, const response_t *resp)
{
    response_fn cb;
    void *user;
    node_t *node;
    if (!p || !p->used) {
        return;
    }
    cb = p->cb;
    user = p->user;
    node = p->node;
    if (node) {
        HASH_DEL(node->pending_hash, p);
    }
    p->used = 0;
    p->node = NULL;
    if (p->data) {
        zri_mem_free(p->data);
        p->data = NULL;
    }
    if (p->metadata) {
        free(p->metadata);
        p->metadata = NULL;
    }
    p->metadata_len = 0;
    /* response_metadata / response_message 由回调借用，必须等 pending_recycle 再释放。 */
    p->has_req = 0;
    p->len = 0;
    if (node) {
        if (status == ERR_TIMEOUT) {
            ZRI_METRIC_INC(node->metrics.timeouts);
        }
        if (status != OK) {
            ZRI_METRIC_INC(node->metrics.requests_failed);
        }
    }
    if (p->streaming) {
        stream_done_fn done = p->done_cb;
        void *stream_user = p->stream_user;
        p->chunk_cb = NULL;
        p->done_cb = NULL;
        if (done) {
            done(status, stream_user);
        }
    } else if (cb) {
        cb(status, resp, user);
    }
    if (node) {
        pending_recycle(node, p);
    }
}

void zri_pending_release(zri_pending_t *p)
{
    node_t *node;
    if (!p || !p->used) {
        return;
    }
    node = p->node;
    if (node) {
        HASH_DEL(node->pending_hash, p);
    }
    if (p->data) {
        zri_mem_free(p->data);
        p->data = NULL;
    }
    if (p->metadata) {
        free(p->metadata);
        p->metadata = NULL;
    }
    p->metadata_len = 0;
    if (p->response_metadata) {
        free(p->response_metadata);
        p->response_metadata = NULL;
    }
    if (p->response_message) {
        free(p->response_message);
        p->response_message = NULL;
    }
    if (node) {
        pending_recycle(node, p);
    } else {
        p->used = 0;
    }
}

zri_pending_t *zri_pending_add(
    node_t *node, uint64_t id, response_fn cb, void *user, uint64_t timeout_ms, int *out_rc)
{
    zri_pending_t *p;
    if (out_rc) {
        *out_rc = OK;
    }
    if (!node) {
        if (out_rc) {
            *out_rc = ERR_INVALID;
        }
        return NULL;
    }
    if (node->max_inflight && node->pending_count >= node->max_inflight) {
        if (out_rc) {
            *out_rc = ERR_AGAIN;
        }
        return NULL;
    }
    if (node->pending_free) {
        p = node->pending_free;
        node->pending_free = p->free_next;
        p->free_next = NULL;
    } else {
        p = (zri_pending_t *)calloc(1, sizeof(*p));
        if (!p) {
            if (out_rc) {
                *out_rc = ERR_NOMEM;
            }
            return NULL;
        }
        p->next = node->pending;
        node->pending = p;
    }
    p->used = 1;
    p->node = node;
    p->owner = NULL;
    p->request_id = id;
    p->cb = cb;
    p->user = user;
    p->deadline_ms = zri_now_ms() + (timeout_ms ? timeout_ms : DEFAULT_TIMEOUT_MS);
    p->has_req = 0;
    p->streaming = 0;
    p->chunk_cb = NULL;
    p->done_cb = NULL;
    p->stream_user = NULL;
    p->data = NULL;
    p->len = 0;
    p->metadata = NULL;
    p->metadata_len = 0;
    p->response_metadata = NULL;
    p->response_message = NULL;
    p->expected_offset = 0;
    p->next_retx_ms = 0;
    p->retries = 0;
    p->bp_retries = 0;
    HASH_ADD(hh, node->pending_hash, request_id, sizeof(p->request_id), p);
    node->pending_count++;
    return p;
}

zri_pending_t *zri_pending_find(node_t *node, uint64_t id)
{
    zri_pending_t *p = NULL;
    if (!node) {
        return NULL;
    }
    HASH_FIND(hh, node->pending_hash, &id, sizeof(id), p);
    if (p && p->used) {
        return p;
    }
    return NULL;
}

void zri_pending_tick(node_t *node, uint64_t now_ms)
{
    zri_pending_t *p;
    if (!node) {
        return;
    }
    for (p = node->pending; p; p = p->next) {
        if (!p->used) {
            continue;
        }
        if (now_ms >= p->deadline_ms) {
            zri_pending_invoke(p, ERR_TIMEOUT, NULL);
            continue;
        }
        /* RTO 超时重传请求（覆盖整包丢失：NACK 无从触发的情况）。
         * 仅用于 UDP：TCP 本身可靠有序，应用层重传只会产生重复请求。 */
        if (p->has_req && p->transport == TRANSPORT_UDP && p->retries < MAX_RETX &&
            now_ms >= p->next_retx_ms) {
            uint32_t shift = p->retries < 4 ? p->retries : 4;
            zri_pending_retx(node, p);
            p->retries++;
            p->next_retx_ms = now_ms + (uint64_t)RTO_INIT_MS * ((uint64_t)1u << shift);
        }
    }
}

void zri_pending_cleanup(node_t *node)
{
    zri_pending_t *p;
    if (!node) {
        return;
    }
    p = node->pending;
    while (p) {
        zri_pending_t *next = p->next;
        if (p->data) {
            zri_mem_free(p->data);
            p->data = NULL;
        }
        if (p->metadata) {
            free(p->metadata);
            p->metadata = NULL;
        }
        if (p->response_metadata) {
            free(p->response_metadata);
            p->response_metadata = NULL;
        }
        if (p->response_message) {
            free(p->response_message);
            p->response_message = NULL;
        }
        free(p);
        p = next;
    }
    node->pending = NULL;
    node->pending_hash = NULL;
    node->pending_free = NULL;
    node->pending_count = 0;
}
