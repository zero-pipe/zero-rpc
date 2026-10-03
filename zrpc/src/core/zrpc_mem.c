/*
 * zrpc 控制对象分配器。
 *
 * 基于当前 poller 的本地分档池分配定长控制对象（call / task 等）。每个对象
 * 前置一个归属头，记录来源池、owner poller 与容量：
 *   - 释放发生在 owner poller 线程：归还本地池，命中 freelist。
 *   - 释放发生在其他线程（如 node_destroy）：直接 free，避免向已停止的 poller
 *     投递延迟归还任务而在池销毁后触发悬垂。
 * 池块本身由 malloc 支撑，因此跨线程直接 free 始终安全，只是放弃该次复用。
 */
#include "zrpc_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct zrpc_mem_hdr {
    ztk_buf_pool *pool;
    ztk_poller *owner;
    size_t cap;
} zrpc_mem_hdr_t;

void *zrpc_mem_alloc(size_t size) {
    zrpc_mem_hdr_t *h;
    ztk_poller *owner = ztk_poller_current();
    ztk_buf_pool *pool = owner ? ztk_poller_buf_pool(owner) : NULL;
    size_t cap = 0;
    void *raw = NULL;

    if (size == 0) {
        return NULL;
    }
    if (pool) {
        raw = ztk_buf_pool_acquire(pool, sizeof(*h) + size, &cap);
    }
    if (!raw) {
        raw = malloc(sizeof(*h) + size);
        if (!raw) {
            return NULL;
        }
        cap = 0;
        pool = NULL;
        owner = NULL;
    }
    h = (zrpc_mem_hdr_t *)raw;
    h->pool = pool;
    h->owner = owner;
    h->cap = cap;
    return (void *)(h + 1);
}

void zrpc_mem_free(void *ptr) {
    zrpc_mem_hdr_t *h;

    if (!ptr) {
        return;
    }
    h = ((zrpc_mem_hdr_t *)ptr) - 1;
    if (h->pool && h->owner && ztk_poller_is_current_thread(h->owner)) {
        ztk_buf_pool_release(h->pool, h, h->cap);
    } else {
        free(h);
    }
}
