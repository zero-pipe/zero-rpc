/* 客户端：代理对象、路由解析、异步调用与取消（流式见 stream.c）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include "envelope.h"
#include "atomic.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct nb_cancel_task {
    nb_proxy_t *proxy;
    uint64_t id;
} nb_cancel_task_t;

int nb_proxy_resolve(nb_proxy_t *proxy,
                      const char *service,
                      char *ip_out,
                      size_t ip_cap,
                      uint16_t *port_out,
                      nb_transport_kind_t *transport_out,
                      char *scheme_out,
                      size_t scheme_cap,
                      int prefer_stream)
{
    nb_stub_target_t target;
    if (!proxy) {
        return NB_ERR_INVALID;
    }
    if (proxy->has_endpoint) {
        nb_copy_str(ip_out, ip_cap, proxy->endpoint_ip, "127.0.0.1");
        *port_out = proxy->endpoint_port;
        if (transport_out) {
            *transport_out = proxy->endpoint_transport;
        }
        if (scheme_out) {
            nb_copy_str(scheme_out,
                         scheme_cap,
                         proxy->endpoint_scheme[0] ?
                             proxy->endpoint_scheme :
                             (proxy->endpoint_transport == NB_TRANSPORT_TCP ? "tcp" : "udp"),
                         "udp");
        }
        return NB_OK;
    }
    if (nb_node_resolve(proxy->node, service, prefer_stream, &target) != NB_OK) {
        return NB_ERR_NOTFOUND;
    }
    nb_copy_str(ip_out, ip_cap, target.ip, "0.0.0.0");
    *port_out = target.port;
    if (transport_out) {
        *transport_out = target.transport;
    }
    if (scheme_out) {
        nb_copy_str(scheme_out, scheme_cap, target.scheme, "udp");
    }
    return NB_OK;
}

/*
 * proxy 注入到 stub 的发送钩子：复用 nb_proxy_resolve 完成端点覆盖/发现，
 * 再把单条消息交给底层传输。
 */
static int nb_proxy_stub_send(nb_stub_t *stub,
                           const char *service,
                           const char *method,
                           const nb_payload_t *payload,
                           nb_stub_kind_t kind,
                           uint64_t request_id,
                           uint64_t timeout_ms,
                           const void *metadata,
                           uint16_t metadata_len,
                           nb_stub_target_t *target,
                           void *user_data)
{
    nb_proxy_t *proxy = (nb_proxy_t *)user_data;
    nb_node_t *node = nb_stub_node(stub);
    nb_transport_kind_t transport = NB_TRANSPORT_UDP;
    int prefer_stream;
    if (!proxy || !node) {
        return NB_ERR_STATE;
    }
    prefer_stream = proxy->prefer_stream_threshold > 0 && payload &&
        payload->len > proxy->prefer_stream_threshold;
    if (nb_proxy_resolve(proxy,
                          service,
                          target->ip,
                          sizeof(target->ip),
                          &target->port,
                          &transport,
                          target->scheme,
                          sizeof(target->scheme),
                          prefer_stream) != NB_OK) {
        return NB_ERR_NOTFOUND;
    }
    target->transport = transport;
    return nb_node_send_call(node,
                              target,
                              service,
                              method,
                              payload,
                              kind == NB_STUB_EVENT ? KIND_EVENT : KIND_REQUEST,
                              request_id,
                              timeout_ms,
                              metadata,
                              metadata_len);
}

static void nb_cancel_task_fn(void *user)
{
    nb_cancel_task_t *t = (nb_cancel_task_t *)user;
    nb_node_t *node = t->proxy->node;
    nb_pending_t *p = nb_pending_find(node, t->id);
    if (p) {
        /*
         * 先通知服务端取消（best-effort），再本地回调。服务端据此跳过仍在排队
         * 的 worker 任务；已运行的 handler 可通过 nb_call_canceled() 自行提前退出。
         */
        if (p->has_req && p->port) {
            nb_stub_target_t target;
            memset(&target, 0, sizeof(target));
            nb_copy_str(target.ip, sizeof(target.ip), p->ip, "0.0.0.0");
            nb_copy_str(target.scheme, sizeof(target.scheme), p->scheme, "udp");
            target.port = p->port;
            target.transport = p->transport;
            (void)nb_node_send_call(
                node, &target, p->service, p->method, NULL, KIND_CANCEL, p->request_id, 0, NULL, 0);
        }
        nb_pending_invoke(p, NB_ERR_NOTFOUND, NULL);
    }
    free(t);
}

/* ---- 生命周期 ---- */

int nb_proxy_create(nb_node_t *node, const nb_proxy_options_t *options, nb_proxy_t **out)
{
    nb_proxy_t *p;
    nb_stub_options_t stub_options;
    if (!node || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    p = (nb_proxy_t *)calloc(1, sizeof(*p));
    if (!p) {
        return NB_ERR_NOMEM;
    }
    p->node = node;
    p->payload_encoding = options ? options->payload_encoding : 0;
    p->prefer_stream_threshold = options ? options->prefer_stream_threshold : 0;
    /* proxy 在 stub 之上注入路由：send 钩子使用 proxy 做解析。 */
    memset(&stub_options, 0, sizeof(stub_options));
    stub_options.payload_encoding = p->payload_encoding;
    stub_options.send = nb_proxy_stub_send;
    stub_options.user_data = p;
    if (nb_stub_create(node, &stub_options, &p->stub) != NB_OK) {
        free(p);
        return NB_ERR_NOMEM;
    }
    *out = p;
    return NB_OK;
}

void nb_proxy_destroy(nb_proxy_t *proxy)
{
    if (!proxy) {
        return;
    }
    /* 先取消该 proxy 名下在途调用；回调在 poller 上完成后再释放 proxy/stub。 */
    if (proxy->stub) {
        (void)nb_stub_cancel_all(proxy->stub, NB_ERR_CLOSED);
    }
    nb_stub_destroy(proxy->stub);
    free(proxy);
}

/* ---- 目标端点 ---- */

int nb_proxy_set_endpoint(nb_proxy_t *proxy, const char *ip, uint16_t port, nb_transport_kind_t transport)
{
    if (!proxy || !ip || !port) {
        return NB_ERR_INVALID;
    }
    nb_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), ip, "127.0.0.1");
    proxy->endpoint_port = port;
    proxy->endpoint_transport = transport;
    nb_copy_str(proxy->endpoint_scheme,
                 sizeof(proxy->endpoint_scheme),
                 transport == NB_TRANSPORT_TCP ? "tcp" : "udp",
                 "udp");
    proxy->has_endpoint = 1;
    return NB_OK;
}

int nb_proxy_set_endpoint_uri(nb_proxy_t *proxy, const char *endpoint)
{
    nb_endpoint_t parsed;
    if (!proxy || !endpoint || nb_endpoint_parse(endpoint, &parsed) != NB_OK || parsed.port == 0) {
        return NB_ERR_INVALID;
    }
    nb_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), parsed.host, "127.0.0.1");
    nb_copy_str(proxy->endpoint_scheme, sizeof(proxy->endpoint_scheme), parsed.scheme, "tcp");
    proxy->endpoint_port = parsed.port;
    proxy->endpoint_transport = parsed.transport;
    proxy->has_endpoint = 1;
    return NB_OK;
}

/* ---- Unary 调用 ---- */

typedef struct nb_proxy_cb_ctx {
    nb_proxy_t *proxy;
    response_fn cb;
    void *user;
} nb_proxy_cb_ctx_t;

static void nb_proxy_response_wrapper(int status, const nb_response_t *resp, void *user)
{
    nb_proxy_cb_ctx_t *ctx = (nb_proxy_cb_ctx_t *)user;
    if (!ctx) {
        return;
    }
    if (ctx->proxy && ctx->proxy->has_interceptor && ctx->proxy->interceptor.after) {
        ctx->proxy->interceptor.after(ctx->proxy->interceptor.ctx, status, resp);
    }
    if (ctx->cb) {
        ctx->cb(status, resp, ctx->user);
    }
    free(ctx);
}

int nb_proxy_set_interceptor(nb_proxy_t *proxy, const nb_proxy_interceptor_t *interceptor)
{
    if (!proxy) {
        return NB_ERR_INVALID;
    }
    if (interceptor) {
        proxy->interceptor = *interceptor;
        proxy->has_interceptor = 1;
    } else {
        memset(&proxy->interceptor, 0, sizeof(proxy->interceptor));
        proxy->has_interceptor = 0;
    }
    return NB_OK;
}

int nb_proxy_call_async_ex(nb_proxy_t *proxy,
                        const char *service,
                        const char *method,
                        const nb_payload_t *payload,
                        const nb_metadata_t *metadata,
                        response_fn cb,
                        void *user,
                        uint64_t timeout_ms,
                        uint64_t *out_request_id)
{
    const nb_metadata_t *use_meta = metadata;
    nb_metadata_t meta;
    nb_proxy_cb_ctx_t *ctx = NULL;
    response_fn use_cb = cb;
    void *use_user = user;
    uint64_t timeout = timeout_ms;
    int rc;

    if (!proxy || !proxy->node || !proxy->stub || !service || !method || !cb) {
        return NB_ERR_INVALID;
    }
    if (proxy->has_interceptor && proxy->interceptor.before) {
        if (metadata) {
            meta = *metadata;
        } else {
            nb_metadata_init(&meta);
        }
        rc = proxy->interceptor.before(proxy->interceptor.ctx, service, method, &meta, &timeout);
        if (rc != NB_OK) {
            return rc;
        }
        use_meta = meta.count ? &meta : NULL;
    }
    if (proxy->has_interceptor && proxy->interceptor.after) {
        ctx = (nb_proxy_cb_ctx_t *)calloc(1, sizeof(*ctx));
        if (!ctx) {
            return NB_ERR_NOMEM;
        }
        ctx->proxy = proxy;
        ctx->cb = cb;
        ctx->user = user;
        use_cb = nb_proxy_response_wrapper;
        use_user = ctx;
    }
    rc = nb_stub_call_ex(proxy->stub,
                          service,
                          method,
                          payload,
                          use_cb,
                          NULL,
                          NULL,
                          0,
                          use_user,
                          timeout,
                          out_request_id,
                          use_meta);
    if (rc != NB_OK && ctx) {
        free(ctx);
    }
    return rc;
}

int nb_proxy_call_async(nb_proxy_t *proxy,
                     const char *service,
                     const char *method,
                     const nb_payload_t *payload,
                     response_fn cb,
                     void *user,
                     uint64_t timeout_ms,
                     uint64_t *out_request_id)
{
    return nb_proxy_call_async_ex(
        proxy, service, method, payload, NULL, cb, user, timeout_ms, out_request_id);
}

int nb_proxy_call_event_ex(nb_proxy_t *proxy,
                        const char *service,
                        const char *method,
                        const nb_payload_t *payload,
                        const nb_metadata_t *metadata,
                        uint64_t *out_request_id)
{
    const nb_metadata_t *use_meta = metadata;
    nb_metadata_t meta;
    int rc;
    if (!proxy || !proxy->node || !proxy->stub || !service || !method) {
        return NB_ERR_INVALID;
    }
    if (proxy->has_interceptor && proxy->interceptor.before) {
        uint64_t timeout = 0;
        if (metadata) {
            meta = *metadata;
        } else {
            nb_metadata_init(&meta);
        }
        rc = proxy->interceptor.before(proxy->interceptor.ctx, service, method, &meta, &timeout);
        if (rc != NB_OK) {
            return rc;
        }
        use_meta = meta.count ? &meta : NULL;
    }
    return nb_stub_call_event_ex(
        proxy->stub, service, method, payload, use_meta, out_request_id);
}

int nb_proxy_call_event(nb_proxy_t *proxy,
                     const char *service,
                     const char *method,
                     const nb_payload_t *payload,
                     uint64_t *out_request_id)
{
    return nb_proxy_call_event_ex(proxy, service, method, payload, NULL, out_request_id);
}

/* ---- 调用控制 ---- */

int nb_proxy_cancel(nb_proxy_t *proxy, uint64_t request_id)
{
    nb_cancel_task_t *t;
    if (!proxy || !proxy->node || !request_id) {
        return NB_ERR_INVALID;
    }
    t = (nb_cancel_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        return NB_ERR_NOMEM;
    }
    t->proxy = proxy;
    t->id = request_id;
    {
        ntk_poller *owner = nb_pending_owner(proxy->node, request_id);
        if (ntk_poller_async(owner ? owner : proxy->node->poller, nb_cancel_task_fn, t, 1) != NTK_OK) {
            free(t);
            return NB_ERR_IO;
        }
    }
    return NB_OK;
}

/* ---- 流式调用 ---- */

int nb_proxy_call_stream(nb_proxy_t *proxy,
                      const char *service,
                      const char *method,
                      const nb_payload_t *request,
                      response_chunk_fn on_chunk,
                      stream_done_fn on_done,
                      void *user,
                      uint64_t timeout_ms,
                      uint64_t *out_request_id)
{
    if (!proxy || !proxy->node || !proxy->stub || !service || !method || !on_chunk || !on_done) {
        return NB_ERR_INVALID;
    }
    return nb_stub_call_ex(proxy->stub,
                            service,
                            method,
                            request,
                            NULL,
                            on_chunk,
                            on_done,
                            1,
                            user,
                            timeout_ms,
                            out_request_id,
                            NULL);
}

/* RTO 重传：按保存的请求参数重发（同一 msg_id）。 */
void nb_pending_retx(nb_node_t *node, nb_pending_t *p)
{
    nb_stub_target_t target;
    nb_payload_t pl;
    uint64_t now;
    uint64_t remaining;
    if (!node || !p || !p->has_req) {
        return;
    }
    memset(&target, 0, sizeof(target));
    nb_copy_str(target.ip, sizeof(target.ip), p->ip, "0.0.0.0");
    nb_copy_str(target.scheme, sizeof(target.scheme), p->scheme, "udp");
    target.port = p->port;
    target.transport = p->transport;
    pl.data = p->data;
    pl.len = p->len;
    pl.encoding = p->encoding;
    now = nb_now_ms();
    remaining = p->deadline_ms > now ? p->deadline_ms - now : 1;
    (void)nb_node_send_call(node,
                             &target,
                             p->service,
                             p->method,
                             &pl,
                             KIND_REQUEST,
                             p->request_id,
                             remaining,
                             p->metadata,
                             p->metadata_len);
    NB_METRIC_INC(node->metrics.retries);
}
