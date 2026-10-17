/* 客户端：代理对象、路由解析、异步调用与取消（流式见 stream.c）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include "envelope.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct zri_cancel_task {
    proxy_t *proxy;
    uint64_t id;
} zri_cancel_task_t;

int zri_proxy_resolve(proxy_t *proxy, const char *service, char *ip_out, size_t ip_cap,
                       uint16_t *port_out, transport_kind_t *transport_out,
                       char *scheme_out, size_t scheme_cap, int prefer_stream) {
    stub_target_t target;
    if (!proxy) {
        return ERR_INVALID;
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
                              : (proxy->endpoint_transport == TRANSPORT_TCP ? "tcp" : "udp"),
                          "udp");
        }
        return OK;
    }
    if (zri_node_resolve(proxy->node, service, prefer_stream, &target) != OK) {
        return ERR_NOTFOUND;
    }
    zri_copy_str(ip_out, ip_cap, target.ip, "0.0.0.0");
    *port_out = target.port;
    if (transport_out) {
        *transport_out = target.transport;
    }
    if (scheme_out) {
        zri_copy_str(scheme_out, scheme_cap, target.scheme, "udp");
    }
    return OK;
}

/*
 * proxy 注入到 stub 的发送钩子：复用 zri_proxy_resolve 完成端点覆盖/发现，
 * 再把单条消息交给底层传输。
 */
static int proxy_stub_send(stub_t *stub, const char *service, const char *method,
                           const payload_t *payload, stub_kind_t kind,
                           uint64_t request_id, stub_target_t *target, void *user_data) {
    proxy_t *proxy = (proxy_t *)user_data;
    node_t *node = stub_node(stub);
    transport_kind_t transport = TRANSPORT_UDP;
    int prefer_stream;
    if (!proxy || !node) {
        return ERR_STATE;
    }
    prefer_stream = proxy->prefer_stream_threshold > 0 && payload &&
                    payload->len > proxy->prefer_stream_threshold;
    if (zri_proxy_resolve(proxy, service, target->ip, sizeof(target->ip), &target->port, &transport,
                           target->scheme, sizeof(target->scheme), prefer_stream) != OK) {
        return ERR_NOTFOUND;
    }
    target->transport = transport;
    return zri_node_send_call(node, target, service, method, payload,
                              kind == STUB_EVENT ? KIND_EVENT : KIND_REQUEST,
                              request_id);
}

static void cancel_task_fn(void *user) {
    zri_cancel_task_t *t = (zri_cancel_task_t *)user;
    zri_pending_t *p = zri_pending_find(t->proxy->node, t->id);
    if (p) {
        zri_pending_invoke(p, ERR_NOTFOUND, NULL);
    }
    free(t);
}

/* ---- 生命周期 ---- */

int proxy_create(node_t *node, const proxy_options_t *options, proxy_t **out) {
    proxy_t *p;
    stub_options_t stub_options;
    if (!node || !out) {
        return ERR_INVALID;
    }
    *out = NULL;
    p = (proxy_t *)calloc(1, sizeof(*p));
    if (!p) {
        return ERR_NOMEM;
    }
    p->node = node;
    p->payload_encoding = options ? options->payload_encoding : 0;
    p->prefer_stream_threshold = options ? options->prefer_stream_threshold : 0;
    /* proxy 在 stub 之上注入路由：send 钩子使用 proxy 做解析。 */
    memset(&stub_options, 0, sizeof(stub_options));
    stub_options.payload_encoding = p->payload_encoding;
    stub_options.send = proxy_stub_send;
    stub_options.user_data = p;
    if (stub_create(node, &stub_options, &p->stub) != OK) {
        free(p);
        return ERR_NOMEM;
    }
    *out = p;
    return OK;
}

void proxy_destroy(proxy_t *proxy) {
    if (!proxy) {
        return;
    }
    stub_destroy(proxy->stub);
    free(proxy);
}

/* ---- 目标端点 ---- */

int proxy_set_endpoint(proxy_t *proxy, const char *ip, uint16_t port,
                            transport_kind_t transport) {
    if (!proxy || !ip || !port) {
        return ERR_INVALID;
    }
    zri_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), ip, "127.0.0.1");
    proxy->endpoint_port = port;
    proxy->endpoint_transport = transport;
    zri_copy_str(proxy->endpoint_scheme, sizeof(proxy->endpoint_scheme),
                  transport == TRANSPORT_TCP ? "tcp" : "udp", "udp");
    proxy->has_endpoint = 1;
    return OK;
}

int proxy_set_endpoint_uri(proxy_t *proxy, const char *endpoint) {
    endpoint_t parsed;
    if (!proxy || !endpoint || endpoint_parse(endpoint, &parsed) != OK ||
        parsed.port == 0) {
        return ERR_INVALID;
    }
    zri_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), parsed.host, "127.0.0.1");
    zri_copy_str(proxy->endpoint_scheme, sizeof(proxy->endpoint_scheme), parsed.scheme, "tcp");
    proxy->endpoint_port = parsed.port;
    proxy->endpoint_transport = parsed.transport;
    proxy->has_endpoint = 1;
    return OK;
}

/* ---- Unary 调用 ---- */

int proxy_call_async(proxy_t *proxy, const char *service, const char *method,
                          const payload_t *payload, response_fn cb, void *user,
                          uint64_t timeout_ms, uint64_t *out_request_id) {
    if (!proxy || !proxy->node || !proxy->stub || !service || !method || !cb) {
        return ERR_INVALID;
    }
    return zri_stub_call_ex(proxy->stub, service, method, payload, cb, NULL, NULL, 0, user,
                             timeout_ms, out_request_id);
}

int proxy_call(proxy_t *proxy, const char *service, const char *method,
                    const payload_t *payload, response_fn cb, void *user,
                    uint64_t timeout_ms, uint64_t *out_request_id) {
    return proxy_call_async(proxy, service, method, payload, cb, user, timeout_ms,
                                 out_request_id);
}

int proxy_call_event(proxy_t *proxy, const char *service, const char *method,
                          const payload_t *payload, uint64_t *out_request_id) {
    if (!proxy || !proxy->node || !proxy->stub || !service || !method) {
        return ERR_INVALID;
    }
    return stub_call(proxy->stub, service, method, payload, out_request_id);
}

/* ---- 调用控制 ---- */

int proxy_cancel(proxy_t *proxy, uint64_t request_id) {
    zri_cancel_task_t *t;
    if (!proxy || !proxy->node || !request_id) {
        return ERR_INVALID;
    }
    t = (zri_cancel_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        return ERR_NOMEM;
    }
    t->proxy = proxy;
    t->id = request_id;
    if (ztk_poller_async(proxy->node->poller, cancel_task_fn, t, 1) != ZTK_OK) {
        free(t);
        return ERR_IO;
    }
    return OK;
}

/* ---- 流式调用 ---- */

int proxy_call_stream(proxy_t *proxy, const char *service, const char *method,
                           const payload_t *request, response_chunk_fn on_chunk,
                           stream_done_fn on_done, void *user, uint64_t timeout_ms,
                           uint64_t *out_request_id) {
    if (!proxy || !proxy->node || !proxy->stub || !service || !method || !on_chunk || !on_done) {
        return ERR_INVALID;
    }
    return zri_stub_call_ex(proxy->stub, service, method, request, NULL, on_chunk, on_done, 1, user,
                             timeout_ms, out_request_id);
}

/* RTO 重传：按保存的请求参数重发（同一 msg_id）。 */
void zri_pending_retx(node_t *node, zri_pending_t *p) {
    stub_target_t target;
    payload_t pl;
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
    (void)zri_node_send_call(node, &target, p->service, p->method, &pl, KIND_REQUEST,
                             p->request_id);
    node->metrics.retries++;
}
