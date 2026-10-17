/* 客户端：代理对象、路由解析、异步调用与取消（流式见 zrpc_stream.c）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include "envelope.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct zri_cancel_task {
    zrpc_proxy_t *proxy;
    uint64_t id;
} zri_cancel_task_t;

int zri_proxy_resolve(zrpc_proxy_t *proxy, const char *service, char *ip_out, size_t ip_cap,
                       uint16_t *port_out, zrpc_transport_kind_t *transport_out,
                       char *scheme_out, size_t scheme_cap, int prefer_stream) {
    zrpc_stub_target_t target;
    if (!proxy) {
        return ZRPC_ERR_INVALID;
    }
    if (proxy->has_endpoint) {
        zri_copy_str(ip_out, ip_cap, proxy->endpoint_ip, "127.0.0.1");
        *port_out = proxy->endpoint_port;
        if (transport_out) {
            *transport_out = proxy->endpoint_transport;
        }
        if (scheme_out) {
            zri_copy_str(scheme_out, scheme_cap,
                          proxy->endpoint_scheme[0]
                              ? proxy->endpoint_scheme
                              : (proxy->endpoint_transport == ZRPC_TRANSPORT_TCP ? "tcp" : "udp"),
                          "udp");
        }
        return ZRPC_OK;
    }
    if (zri_node_resolve(proxy->node, service, prefer_stream, &target) != ZRPC_OK) {
        return ZRPC_ERR_NOTFOUND;
    }
    zri_copy_str(ip_out, ip_cap, target.ip, "0.0.0.0");
    *port_out = target.port;
    if (transport_out) {
        *transport_out = target.transport;
    }
    if (scheme_out) {
        zri_copy_str(scheme_out, scheme_cap, target.scheme, "udp");
    }
    return ZRPC_OK;
}

/*
 * proxy 注入到 stub 的发送钩子：复用 zri_proxy_resolve 完成端点覆盖/发现，
 * 再把单条消息交给底层传输。
 */
static int proxy_stub_send(zrpc_stub_t *stub, const char *service, const char *method,
                           const zrpc_payload_t *payload, zrpc_stub_kind_t kind,
                           uint64_t request_id, zrpc_stub_target_t *target, void *user_data) {
    zrpc_proxy_t *proxy = (zrpc_proxy_t *)user_data;
    zrpc_node_t *node = zrpc_stub_node(stub);
    zrpc_transport_kind_t transport = ZRPC_TRANSPORT_UDP;
    int prefer_stream;
    if (!proxy || !node) {
        return ZRPC_ERR_STATE;
    }
    prefer_stream = proxy->prefer_stream_threshold > 0 && payload &&
                    payload->len > proxy->prefer_stream_threshold;
    if (zri_proxy_resolve(proxy, service, target->ip, sizeof(target->ip), &target->port, &transport,
                           target->scheme, sizeof(target->scheme), prefer_stream) != ZRPC_OK) {
        return ZRPC_ERR_NOTFOUND;
    }
    target->transport = transport;
    return zri_node_send_call(node, target, service, method, payload,
                              kind == ZRPC_STUB_EVENT ? ZRPC_KIND_EVENT : ZRPC_KIND_REQUEST,
                              request_id);
}

static void cancel_task_fn(void *user) {
    zri_cancel_task_t *t = (zri_cancel_task_t *)user;
    zri_pending_t *p = zri_pending_find(t->proxy->node, t->id);
    if (p) {
        zri_pending_invoke(p, ZRPC_ERR_NOTFOUND, NULL);
    }
    free(t);
}

/* ---- 生命周期 ---- */

int zrpc_proxy_create(zrpc_node_t *node, const zrpc_proxy_options_t *options, zrpc_proxy_t **out) {
    zrpc_proxy_t *p;
    zrpc_stub_options_t stub_options;
    if (!node || !out) {
        return ZRPC_ERR_INVALID;
    }
    *out = NULL;
    p = (zrpc_proxy_t *)calloc(1, sizeof(*p));
    if (!p) {
        return ZRPC_ERR_NOMEM;
    }
    p->node = node;
    p->payload_encoding = options ? options->payload_encoding : 0;
    p->prefer_stream_threshold = options ? options->prefer_stream_threshold : 0;
    /* proxy 在 stub 之上注入路由：send 钩子使用 proxy 做解析。 */
    memset(&stub_options, 0, sizeof(stub_options));
    stub_options.payload_encoding = p->payload_encoding;
    stub_options.send = proxy_stub_send;
    stub_options.user_data = p;
    if (zrpc_stub_create(node, &stub_options, &p->stub) != ZRPC_OK) {
        free(p);
        return ZRPC_ERR_NOMEM;
    }
    *out = p;
    return ZRPC_OK;
}

void zrpc_proxy_destroy(zrpc_proxy_t *proxy) {
    if (!proxy) {
        return;
    }
    zrpc_stub_destroy(proxy->stub);
    free(proxy);
}

/* ---- 目标端点 ---- */

int zrpc_proxy_set_endpoint(zrpc_proxy_t *proxy, const char *ip, uint16_t port,
                            zrpc_transport_kind_t transport) {
    if (!proxy || !ip || !port) {
        return ZRPC_ERR_INVALID;
    }
    zri_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), ip, "127.0.0.1");
    proxy->endpoint_port = port;
    proxy->endpoint_transport = transport;
    zri_copy_str(proxy->endpoint_scheme, sizeof(proxy->endpoint_scheme),
                  transport == ZRPC_TRANSPORT_TCP ? "tcp" : "udp", "udp");
    proxy->has_endpoint = 1;
    return ZRPC_OK;
}

int zrpc_proxy_set_endpoint_uri(zrpc_proxy_t *proxy, const char *endpoint) {
    zrpc_endpoint_t parsed;
    if (!proxy || !endpoint || zrpc_endpoint_parse(endpoint, &parsed) != ZRPC_OK ||
        parsed.port == 0) {
        return ZRPC_ERR_INVALID;
    }
    zri_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), parsed.host, "127.0.0.1");
    zri_copy_str(proxy->endpoint_scheme, sizeof(proxy->endpoint_scheme), parsed.scheme, "tcp");
    proxy->endpoint_port = parsed.port;
    proxy->endpoint_transport = parsed.transport;
    proxy->has_endpoint = 1;
    return ZRPC_OK;
}

/* ---- Unary 调用 ---- */

int zrpc_proxy_call_async(zrpc_proxy_t *proxy, const char *service, const char *method,
                          const zrpc_payload_t *payload, zrpc_response_fn cb, void *user,
                          uint64_t timeout_ms, uint64_t *out_request_id) {
    if (!proxy || !proxy->node || !proxy->stub || !service || !method || !cb) {
        return ZRPC_ERR_INVALID;
    }
    return zri_stub_call_ex(proxy->stub, service, method, payload, cb, NULL, NULL, 0, user,
                             timeout_ms, out_request_id);
}

int zrpc_proxy_call(zrpc_proxy_t *proxy, const char *service, const char *method,
                    const zrpc_payload_t *payload, zrpc_response_fn cb, void *user,
                    uint64_t timeout_ms, uint64_t *out_request_id) {
    return zrpc_proxy_call_async(proxy, service, method, payload, cb, user, timeout_ms,
                                 out_request_id);
}

int zrpc_proxy_call_event(zrpc_proxy_t *proxy, const char *service, const char *method,
                          const zrpc_payload_t *payload, uint64_t *out_request_id) {
    if (!proxy || !proxy->node || !proxy->stub || !service || !method) {
        return ZRPC_ERR_INVALID;
    }
    return zrpc_stub_call(proxy->stub, service, method, payload, out_request_id);
}

/* ---- 调用控制 ---- */

int zrpc_proxy_cancel(zrpc_proxy_t *proxy, uint64_t request_id) {
    zri_cancel_task_t *t;
    if (!proxy || !proxy->node || !request_id) {
        return ZRPC_ERR_INVALID;
    }
    t = (zri_cancel_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        return ZRPC_ERR_NOMEM;
    }
    t->proxy = proxy;
    t->id = request_id;
    if (ztk_poller_async(proxy->node->poller, cancel_task_fn, t, 1) != ZTK_OK) {
        free(t);
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

/* ---- 流式调用 ---- */

int zrpc_proxy_call_stream(zrpc_proxy_t *proxy, const char *service, const char *method,
                           const zrpc_payload_t *request, zrpc_response_chunk_fn on_chunk,
                           zrpc_stream_done_fn on_done, void *user, uint64_t timeout_ms,
                           uint64_t *out_request_id) {
    if (!proxy || !proxy->node || !proxy->stub || !service || !method || !on_chunk || !on_done) {
        return ZRPC_ERR_INVALID;
    }
    return zri_stub_call_ex(proxy->stub, service, method, request, NULL, on_chunk, on_done, 1, user,
                             timeout_ms, out_request_id);
}

/* RTO 重传：按保存的请求参数重发（同一 msg_id）。 */
void zri_pending_retx(zrpc_node_t *node, zri_pending_t *p) {
    zrpc_stub_target_t target;
    zrpc_payload_t pl;
    if (!node || !p || !p->has_req) {
        return;
    }
    memset(&target, 0, sizeof(target));
    zri_copy_str(target.ip, sizeof(target.ip), p->ip, "0.0.0.0");
    zri_copy_str(target.scheme, sizeof(target.scheme), p->scheme, "udp");
    target.port = p->port;
    target.transport = p->transport;
    pl.data = p->data;
    pl.len = p->len;
    pl.encoding = p->encoding;
    (void)zri_node_send_call(node, &target, p->service, p->method, &pl, ZRPC_KIND_REQUEST,
                             p->request_id);
    node->metrics.retries++;
}
