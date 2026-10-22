/* 同步调用便捷层：在异步 API 之上加阻塞等待（禁止在 poller 线程使用）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct nb_sync_ctx {
    ntk_sem *sem;
    ntk_mutex *lock;
    int refs; /* 2：等待方 + 回调方，最后一个释放 */
    int done;
    int status;     /* 传输状态 */
    int app_status; /* 应用状态码 */
    void *data;
    size_t len;
    uint8_t encoding;
    nb_metadata_t *metadata;
    char *message;
} nb_sync_ctx_t;

static void nb_sync_ctx_put(nb_sync_ctx_t *c)
{
    int last;
    ntk_mutex_lock(c->lock);
    last = (--c->refs == 0);
    ntk_mutex_unlock(c->lock);
    if (last) {
        if (c->data) {
            free(c->data);
        }
        if (c->metadata) {
            free(c->metadata);
        }
        if (c->message) {
            free(c->message);
        }
        ntk_mutex_destroy(c->lock);
        ntk_sem_destroy(c->sem);
        free(c);
    }
}

static void nb_sync_response_cb(int status, const nb_response_t *resp, void *user)
{
    nb_sync_ctx_t *c = (nb_sync_ctx_t *)user;
    ntk_mutex_lock(c->lock);
    if (c->done) {
        ntk_mutex_unlock(c->lock);
        nb_sync_ctx_put(c);
        return;
    }
    c->status = status;
    if (status == NB_OK && resp) {
        int oom = 0;
        c->app_status = resp->status;
        c->encoding = resp->payload.encoding;
        if (resp->metadata && resp->metadata->count > 0) {
            c->metadata = (nb_metadata_t *)malloc(sizeof(*c->metadata));
            if (c->metadata) {
                *c->metadata = *resp->metadata;
            } else {
                oom = 1;
            }
        }
        if (!oom && resp->message && resp->message[0]) {
            size_t n = strlen(resp->message) + 1;
            c->message = (char *)malloc(n);
            if (c->message) {
                memcpy(c->message, resp->message, n);
            } else {
                oom = 1;
            }
        }
        if (!oom && resp->payload.len > 0 && resp->payload.data) {
            c->data = malloc(resp->payload.len);
            if (c->data) {
                memcpy(c->data, resp->payload.data, resp->payload.len);
                c->len = resp->payload.len;
            } else {
                oom = 1;
            }
        }
        if (oom) {
            free(c->data);
            c->data = NULL;
            c->len = 0;
            free(c->metadata);
            c->metadata = NULL;
            free(c->message);
            c->message = NULL;
            c->status = NB_ERR_NOMEM;
        }
    }
    c->done = 1;
    ntk_mutex_unlock(c->lock);
    ntk_sem_post(c->sem, 1);
    nb_sync_ctx_put(c);
}

/* ---- 同步调用 ---- */

int nb_proxy_call_sync_ex(nb_proxy_t *proxy,
                       const char *service,
                       const char *method,
                       const nb_payload_t *request,
                       const nb_metadata_t *metadata,
                       nb_response_t *response,
                       uint64_t timeout_ms,
                       uint64_t *out_request_id)
{
    nb_sync_ctx_t *c;
    uint64_t id = 0;
    int rc;

    if (!proxy || !service || !method || !response) {
        return NB_ERR_INVALID;
    }
    memset(response, 0, sizeof(*response));
    c = (nb_sync_ctx_t *)calloc(1, sizeof(*c));
    if (!c) {
        return NB_ERR_NOMEM;
    }
    c->sem = ntk_sem_create(0);
    c->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!c->sem || !c->lock) {
        if (c->sem) {
            ntk_sem_destroy(c->sem);
        }
        if (c->lock) {
            ntk_mutex_destroy(c->lock);
        }
        free(c);
        return NB_ERR_NOMEM;
    }
    c->refs = 2;

    rc = nb_proxy_call_async_ex(
        proxy, service, method, request, metadata, nb_sync_response_cb, c, timeout_ms, &id);
    if (rc != NB_OK) {
        nb_sync_ctx_put(c);
        nb_sync_ctx_put(c);
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    if (ntk_sem_timedwait(c->sem, (unsigned)(timeout_ms ? timeout_ms : NB_DEFAULT_TIMEOUT_MS)) !=
        NTK_OK) {
        /* 取消未完成调用；回调（若仍会到达）会释放它的引用。 */
        (void)nb_proxy_cancel(proxy, id);
        nb_sync_ctx_put(c);
        return NB_ERR_TIMEOUT;
    }
    if (c->status == NB_OK) {
        response->status = c->app_status;
        response->message = c->message;
        response->metadata = c->metadata;
        response->payload.data = c->data;
        response->payload.len = c->len;
        response->payload.encoding = c->encoding;
        c->data = NULL; /* 所有权转移给调用方 */
        c->message = NULL;
        c->metadata = NULL;
        nb_sync_ctx_put(c);
        return NB_OK;
    }
    rc = c->status;
    nb_sync_ctx_put(c);
    return rc;
}

int nb_proxy_call_sync(nb_proxy_t *proxy,
                    const char *service,
                    const char *method,
                    const nb_payload_t *request,
                    nb_response_t *response,
                    uint64_t timeout_ms,
                    uint64_t *out_request_id)
{
    return nb_proxy_call_sync_ex(
        proxy, service, method, request, NULL, response, timeout_ms, out_request_id);
}

void nb_response_release(nb_response_t *response)
{
    if (!response) {
        return;
    }
    free((void *)response->payload.data);
    free((void *)response->metadata);
    free((void *)response->message);
    response->payload.data = NULL;
    response->payload.len = 0;
    response->payload.encoding = 0;
    response->status = 0;
    response->metadata = NULL;
    response->message = NULL;
}
