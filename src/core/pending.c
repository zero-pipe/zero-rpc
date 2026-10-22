/*
 * pending：客户端未完成调用的关联表（request_id -> 回调/超时）。
 *
 * 按归属 poller 分片：每个调用在发起它的 poller 上完成超时、重传与回调；
 * 其他 poller 收到响应时先查归属 poller，再把消息回投过去。哈希表由
 * pending_lock 保护，lane 链表只在归属 poller 上访问。
 */
#include "pending.h"

#include "atomic.h"
#include "mem.h"
#include "node_internal.h"

#include <stdlib.h>

static int nb_lane_index(nb_node_t *node, ntk_poller *poller)
{
    unsigned i;
    if (!node) {
        return 0;
    }
    if (poller) {
        for (i = 0; i < node->io_count; i++) {
            if (node->io_pollers[i] == poller) {
                return (int)i;
            }
        }
    }
    return 0;
}

static void nb_lane_insert(nb_node_t *node, nb_pending_t *p)
{
    int idx = nb_lane_index(node, p->poller);
    p->lane_prev = NULL;
    p->lane_next = node->pending_lanes[idx];
    if (p->lane_next) {
        p->lane_next->lane_prev = p;
    }
    node->pending_lanes[idx] = p;
}

static void nb_lane_remove(nb_node_t *node, nb_pending_t *p)
{
    int idx = nb_lane_index(node, p->poller);
    if (p->lane_prev) {
        p->lane_prev->lane_next = p->lane_next;
    } else if (node->pending_lanes[idx] == p) {
        node->pending_lanes[idx] = p->lane_next;
    }
    if (p->lane_next) {
        p->lane_next->lane_prev = p->lane_prev;
    }
    p->lane_prev = NULL;
    p->lane_next = NULL;
}

/* 从哈希与 lane 摘除并递减计数；仅在归属 poller 上调用。 */
static void nb_pending_detach(nb_node_t *node, nb_pending_t *p)
{
    if (node->pending_lock) {
        ntk_mutex_lock(node->pending_lock);
    }
    HASH_DEL(node->pending_hash, p);
    if (node->pending_lock) {
        ntk_mutex_unlock(node->pending_lock);
    }
    nb_lane_remove(node, p);
    p->used = 0;
    nb_atomic_sub_u32((uint32_t *)&node->pending_count, 1u);
}

static void nb_pending_recycle(nb_node_t *node, nb_pending_t *p)
{
    p->node = NULL;
    p->owner = NULL;
    p->poller = NULL;
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
    /* 回收槽位前释放响应 metadata/status message（此时回调已结束）。 */
    if (p->response_metadata) {
        free(p->response_metadata);
        p->response_metadata = NULL;
    }
    if (p->response_message) {
        free(p->response_message);
        p->response_message = NULL;
    }
    p->response_metadata = NULL;
    p->response_message = NULL;
    p->has_req = 0;
    if (node->pending_lock) {
        ntk_mutex_lock(node->pending_lock);
    }
    p->free_next = node->pending_free;
    node->pending_free = p;
    if (node->pending_lock) {
        ntk_mutex_unlock(node->pending_lock);
    }
}

void nb_pending_invoke(nb_pending_t *p, int status, const nb_response_t *resp)
{
    response_fn cb;
    void *user;
    nb_node_t *node;
    if (!p || !p->used) {
        return;
    }
    cb = p->cb;
    user = p->user;
    node = p->node;
    if (node) {
        nb_pending_detach(node, p);
    } else {
        p->used = 0;
    }
    if (p->data) {
        nb_mem_free(p->data);
        p->data = NULL;
    }
    if (p->metadata) {
        free(p->metadata);
        p->metadata = NULL;
    }
    p->metadata_len = 0;
    p->has_req = 0;
    p->len = 0;
    if (node) {
        if (status == NB_ERR_TIMEOUT) {
            NB_METRIC_INC(node->metrics.timeouts);
        }
        if (status != NB_OK) {
            NB_METRIC_INC(node->metrics.requests_failed);
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
        nb_pending_recycle(node, p);
    }
}

void nb_pending_release(nb_pending_t *p)
{
    nb_node_t *node;
    if (!p || !p->used) {
        return;
    }
    node = p->node;
    if (node) {
        nb_pending_detach(node, p);
    } else {
        p->used = 0;
    }
    if (p->data) {
        nb_mem_free(p->data);
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
        nb_pending_recycle(node, p);
    }
}

nb_pending_t *nb_pending_add(nb_node_t *node,
                               uint64_t id,
                               response_fn cb,
                               void *user,
                               uint64_t timeout_ms,
                               ntk_poller *poller,
                               int *out_rc)
{
    nb_pending_t *p;
    if (out_rc) {
        *out_rc = NB_OK;
    }
    if (!node) {
        if (out_rc) {
            *out_rc = NB_ERR_INVALID;
        }
        return NULL;
    }
    if (node->max_inflight &&
        nb_atomic_load_u32((const uint32_t *)&node->pending_count) >= node->max_inflight) {
        if (out_rc) {
            *out_rc = NB_ERR_AGAIN;
        }
        return NULL;
    }
    if (node->pending_lock) {
        ntk_mutex_lock(node->pending_lock);
    }
    if (node->pending_free) {
        p = node->pending_free;
        node->pending_free = p->free_next;
        p->free_next = NULL;
    } else {
        p = (nb_pending_t *)calloc(1, sizeof(*p));
        if (!p) {
            if (node->pending_lock) {
                ntk_mutex_unlock(node->pending_lock);
            }
            if (out_rc) {
                *out_rc = NB_ERR_NOMEM;
            }
            return NULL;
        }
        p->next = node->pending;
        node->pending = p;
    }
    p->used = 1;
    p->node = node;
    p->poller = poller ? poller : node->poller;
    p->owner = NULL;
    p->gen++;
    p->request_id = id;
    p->cb = cb;
    p->user = user;
    p->started_ms = nb_now_ms();
    p->deadline_ms = p->started_ms + (timeout_ms ? timeout_ms : NB_DEFAULT_TIMEOUT_MS);
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
    p->lane_prev = NULL;
    p->lane_next = NULL;
    HASH_ADD(hh, node->pending_hash, request_id, sizeof(p->request_id), p);
    nb_atomic_add_u32((uint32_t *)&node->pending_count, 1u);
    if (node->pending_lock) {
        ntk_mutex_unlock(node->pending_lock);
    }
    nb_lane_insert(node, p);
    return p;
}

nb_pending_t *nb_pending_find(nb_node_t *node, uint64_t id)
{
    nb_pending_t *p = NULL;
    if (!node) {
        return NULL;
    }
    if (node->pending_lock) {
        ntk_mutex_lock(node->pending_lock);
    }
    HASH_FIND(hh, node->pending_hash, &id, sizeof(id), p);
    if (p && !p->used) {
        p = NULL;
    }
    if (node->pending_lock) {
        ntk_mutex_unlock(node->pending_lock);
    }
    return p;
}

ntk_poller *nb_pending_owner(nb_node_t *node, uint64_t id)
{
    nb_pending_t *p = NULL;
    ntk_poller *owner = NULL;
    if (!node) {
        return NULL;
    }
    if (node->pending_lock) {
        ntk_mutex_lock(node->pending_lock);
    }
    HASH_FIND(hh, node->pending_hash, &id, sizeof(id), p);
    if (p && p->used) {
        owner = p->poller;
    }
    if (node->pending_lock) {
        ntk_mutex_unlock(node->pending_lock);
    }
    return owner;
}

/*
 * 先把本 poller lane 上超时/待重传的槽位摘到临时链表，再逐个处理；
 * 这样可以避免回调里取消/新增 pending 破坏正在遍历的 lane。
 */
void nb_pending_tick(nb_node_t *node, uint64_t now_ms, ntk_poller *poller)
{
    int idx;
    nb_pending_t *expired = NULL;
    nb_pending_t *expired_tail = NULL;
    nb_pending_t *p;
    if (!node) {
        return;
    }
    idx = nb_lane_index(node, poller);
    p = node->pending_lanes[idx];
    while (p) {
        nb_pending_t *next = p->lane_next;
        if (p->used) {
            int timeout = (now_ms >= p->deadline_ms);
            int retx = (!timeout && p->has_req && p->transport == NB_TRANSPORT_UDP &&
                        p->retries < MAX_RETX && now_ms >= p->next_retx_ms);
            if (timeout || retx) {
                nb_lane_remove(node, p);
                p->lane_prev = NULL;
                p->lane_next = NULL;
                if (expired_tail) {
                    expired_tail->lane_next = p;
                } else {
                    expired = p;
                }
                expired_tail = p;
            }
        }
        p = next;
    }
    while (expired) {
        nb_pending_t *cur = expired;
        expired = cur->lane_next;
        if (!cur->used) {
            continue;
        }
        if (now_ms >= cur->deadline_ms) {
            nb_pending_invoke(cur, NB_ERR_TIMEOUT, NULL);
        } else {
            uint32_t shift = cur->retries < 4 ? cur->retries : 4;
            nb_pending_retx(node, cur);
            cur->retries++;
            cur->next_retx_ms = now_ms + (uint64_t)RTO_INIT_MS * ((uint64_t)1u << shift);
            nb_lane_insert(node, cur);
        }
    }
}

void nb_pending_cleanup(nb_node_t *node)
{
    nb_pending_t *p;
    unsigned i;
    if (!node) {
        return;
    }
    p = node->pending;
    while (p) {
        nb_pending_t *next = p->next;
        if (p->data) {
            nb_mem_free(p->data);
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
    for (i = 0; i < IO_MAX; i++) {
        node->pending_lanes[i] = NULL;
    }
}
