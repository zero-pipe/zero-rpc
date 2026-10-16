/* 同步调用便捷层：在异步 API 之上加阻塞等待（禁止在 poller 线程使用）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct zri_sync_ctx {
    ztk_sem *sem;
    ztk_mutex *lock;
    int refs; /* 2：等待方 + 回调方，最后一个释放 */
    int done;
    int status;     /* 传输状态 */
    int app_status; /* 应用状态码 */
    void *data;
    size_t len;
    uint8_t encoding;
} zri_sync_ctx_t;

static void sync_ctx_put(zri_sync_ctx_t *c) {
    int last;
    ztk_mutex_lock(c->lock);
    last = (--c->refs == 0);
    ztk_mutex_unlock(c->lock);
    if (last) {
        if (c->data) {
            free(c->data);
        }
        ztk_mutex_destroy(c->lock);
        ztk_sem_destroy(c->sem);
        free(c);
    }
}

static void sync_response_cb(int status, const zrpc_response_t *resp, void *user) {
    zri_sync_ctx_t *c = (zri_sync_ctx_t *)user;
    ztk_mutex_lock(c->lock);
    if (c->done) {
        ztk_mutex_unlock(c->lock);
        sync_ctx_put(c);
        return;
    }
    c->status = status;
    if (status == ZRPC_OK && resp) {
        c->app_status = resp->status;
        c->encoding = resp->payload.encoding;
        if (resp->payload.len > 0 && resp->payload.data) {
            c->data = malloc(resp->payload.len);
            if (c->data) {
                memcpy(c->data, resp->payload.data, resp->payload.len);
                c->len = resp->payload.len;
            }
        }
    }
    c->done = 1;
    ztk_mutex_unlock(c->lock);
    ztk_sem_post(c->sem, 1);
    sync_ctx_put(c);
}

int zrpc_proxy_call_sync(zrpc_proxy_t *proxy, const char *service, const char *method,
                         const zrpc_payload_t *request, zrpc_response_t *response,
                         uint64_t timeout_ms, uint64_t *out_request_id) {
    zri_sync_ctx_t *c;
    uint64_t id = 0;
    int rc;

    if (!proxy || !service || !method || !response) {
        return ZRPC_ERR_INVALID;
    }
    memset(response, 0, sizeof(*response));
    c = (zri_sync_ctx_t *)calloc(1, sizeof(*c));
    if (!c) {
        return ZRPC_ERR_NOMEM;
    }
    c->sem = ztk_sem_create(0);
    c->lock = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    if (!c->sem || !c->lock) {
        if (c->sem) {
            ztk_sem_destroy(c->sem);
        }
        if (c->lock) {
            ztk_mutex_destroy(c->lock);
        }
        free(c);
        return ZRPC_ERR_NOMEM;
    }
    c->refs = 2;

    rc = zrpc_proxy_call(proxy, service, method, request, sync_response_cb, c, timeout_ms, &id);
    if (rc != ZRPC_OK) {
        sync_ctx_put(c);
        sync_ctx_put(c);
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    if (ztk_sem_timedwait(c->sem,
                          (unsigned)(timeout_ms ? timeout_ms : ZRPC_DEFAULT_TIMEOUT_MS)) != ZTK_OK) {
        /* 取消未完成调用；回调（若仍会到达）会释放它的引用。 */
        (void)zrpc_proxy_cancel(proxy, id);
        sync_ctx_put(c);
        return ZRPC_ERR_TIMEOUT;
    }
    if (c->status == ZRPC_OK) {
        response->status = c->app_status;
        response->payload.data = c->data;
        response->payload.len = c->len;
        response->payload.encoding = c->encoding;
        c->data = NULL; /* 所有权转移给调用方 */
        sync_ctx_put(c);
        return ZRPC_OK;
    }
    rc = c->status;
    sync_ctx_put(c);
    return rc;
}

void zrpc_response_release(zrpc_response_t *response) {
    if (!response) {
        return;
    }
    free((void *)response->payload.data);
    response->payload.data = NULL;
    response->payload.len = 0;
    response->payload.encoding = 0;
    response->status = 0;
}
