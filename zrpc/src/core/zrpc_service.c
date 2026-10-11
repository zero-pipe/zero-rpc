/* 服务端：方法注册与请求分发（回复见 zrpc_reply.c）。 */
#include "zrpc_node_internal.h"
#include "zrpc_service_internal.h"

#include "zrpc_mem.h"
#include "zrpc_util.h"

#include <stdlib.h>
#include <string.h>

/* 同步风格 handler 的桥接：返回值即状态码，把视图回复后再释放。 */
typedef struct zrpc_sync_bridge {
    zrpc_sync_handler_fn fn;
    void *user;
} zrpc_sync_bridge_t;

static void sync_method_bridge(zrpc_call_t *call, const zrpc_request_t *req, void *user) {
    zrpc_sync_bridge_t *bridge = (zrpc_sync_bridge_t *)user;
    zrpc_message_view_t view;
    zrpc_payload_t pl;
    int status;
    zrpc_message_view_init(&view);
    status = bridge->fn(req, &view, bridge->user);
    zrpc_message_view_to_payload(&view, &pl);
    zrpc_reply(call, status, &pl);
    zrpc_message_view_release(&view);
}

typedef struct zrpc_svc_task {
    zrpc_node_t *node;
    zrpc_service_t *svc;
    zrpc_method_t *method;
} zrpc_svc_task_t;

static void svc_link_task(void *user) {
    zrpc_svc_task_t *t = (zrpc_svc_task_t *)user;
    t->svc->next = t->node->services;
    t->node->services = t->svc;
    if (t->node->registry_backend && t->node->registry_backend->ops->announce) {
        t->node->registry_backend->ops->announce(t->node, 0);
    }
    free(t);
}

static void svc_method_task(void *user) {
    zrpc_svc_task_t *t = (zrpc_svc_task_t *)user;
    t->method->next = t->svc->methods;
    t->svc->methods = t->method;
    free(t);
}

void zrpc_service_handle_request(zrpc_node_t *node, zrpc_peer_token_t token, const char *ip,
                                 zrpc_transport_t *transport, uint16_t port, uint32_t msg_id,
                                 const char *route,
                                 const zrpc_payload_t *payload) {
    char tmp[ZRPC_MAX_ROUTE];
    char *dot;
    zrpc_service_t *svc;
    zrpc_method_t *m;
    zrpc_handler_fn fn = NULL;
    void *user = NULL;

    zrpc_copy_str(tmp, sizeof(tmp), route, "");
    dot = strchr(tmp, '.');
    if (!dot) {
        zrpc_service_send_status(transport, token, ip, port, msg_id, ZRPC_ERR_NOTFOUND);
        return;
    }
    *dot = '\0';
    for (svc = node->services; svc; svc = svc->next) {
        if (strcmp(svc->name, tmp) != 0) {
            continue;
        }
        for (m = svc->methods; m; m = m->next) {
            if (strcmp(m->name, dot + 1) == 0 && m->fn) {
                fn = m->fn;
                user = m->user;
                break;
            }
        }
        break;
    }
    if (!fn) {
        zrpc_service_send_status(transport, token, ip, port, msg_id, ZRPC_ERR_NOTFOUND);
        return;
    }
    {
        zrpc_call_t *call = (zrpc_call_t *)zrpc_mem_alloc(sizeof(*call));
        zrpc_request_t req;
        if (!call) {
            zrpc_service_send_status(transport, token, ip, port, msg_id, ZRPC_ERR_NOMEM);
            return;
        }
        memset(call, 0, sizeof(*call));
        call->node = node;
        call->transport = transport;
        call->token = token;
        zrpc_copy_str(call->peer_ip, sizeof(call->peer_ip), ip, "0.0.0.0");
        call->peer_port = port;
        call->msg_id = msg_id;
        req.service = tmp;
        req.method = dot + 1;
        req.payload = payload ? *payload : (zrpc_payload_t){0};
        req.peer_ip = call->peer_ip;
        req.peer_port = port;
        fn(call, &req, user);
    }
}

static zrpc_recv_stream_t *find_recv_stream(zrpc_node_t *node, uint32_t request_id,
                                            const char *ip, uint16_t port) {
    int i;
    for (i = 0; i < ZRPC_MAX_RECV_STREAMS; i++) {
        zrpc_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && stream->request_id == request_id && stream->peer_port == port &&
            strcmp(stream->peer_ip, ip ? ip : "") == 0) {
            return stream;
        }
    }
    return NULL;
}

static zrpc_recv_stream_t *alloc_recv_stream(zrpc_node_t *node, uint32_t request_id,
                                             const char *ip, uint16_t port,
                                             zrpc_transport_t *transport, zrpc_peer_token_t token) {
    int i;
    for (i = 0; i < ZRPC_MAX_RECV_STREAMS; i++) {
        zrpc_recv_stream_t *stream = &node->recv_streams[i];
        if (!stream->used) {
            zrpc_call_t *call = (zrpc_call_t *)zrpc_mem_alloc(sizeof(*call));
            if (!call) {
                return NULL;
            }
            memset(call, 0, sizeof(*call));
            stream->used = 1;
            stream->request_id = request_id;
            zrpc_copy_str(stream->peer_ip, sizeof(stream->peer_ip), ip, "0.0.0.0");
            stream->peer_port = port;
            stream->call = call;
            stream->last_ms = zrpc_now_ms();
            stream->bytes = 0;
            call->node = node;
            call->transport = transport;
            call->token = token;
            zrpc_copy_str(call->peer_ip, sizeof(call->peer_ip), ip, "0.0.0.0");
            call->peer_port = port;
            call->msg_id = request_id;
            call->stream_owned = 1;
            return stream;
        }
    }
    return NULL;
}

void zrpc_service_handle_stream_chunk(zrpc_node_t *node, zrpc_peer_token_t token, const char *ip,
                                      zrpc_transport_t *transport, uint16_t port, uint32_t msg_id,
                                      const char *route, const zrpc_stream_meta_t *stream_meta,
                                      const zrpc_payload_t *payload) {
    char tmp[ZRPC_MAX_ROUTE];
    char *dot;
    zrpc_service_t *svc;
    zrpc_method_t *method = NULL;
    zrpc_recv_stream_t *state;
    zrpc_request_t request;
    zrpc_chunk_t chunk;

    if (!node || !stream_meta) {
        return;
    }
    state = find_recv_stream(node, msg_id, ip, port);

    if (stream_meta->flags & ZRPC_STREAM_CANCEL) {
        if (state) {
            zrpc_mem_free(state->call);
            memset(state, 0, sizeof(*state));
        }
        return;
    }

    if (state) {
        method = state->method;
    } else {
        if (!(stream_meta->flags & ZRPC_STREAM_FIRST)) {
            return; /* 没有 FIRST 也没有已有状态：协议错误 */
        }
        zrpc_copy_str(tmp, sizeof(tmp), route, "");
        dot = strchr(tmp, '.');
        if (!dot) {
            return;
        }
        *dot = '\0';
        for (svc = node->services; svc; svc = svc->next) {
            if (strcmp(svc->name, tmp) != 0) {
                continue;
            }
            for (method = svc->methods; method; method = method->next) {
                if (strcmp(method->name, dot + 1) == 0 && method->stream_fn) {
                    break;
                }
            }
            break;
        }
        if (!method) {
            return;
        }
        state = alloc_recv_stream(node, msg_id, ip, port, transport, token);
        if (state) {
            state->method = method;
            zrpc_copy_str(state->service, sizeof(state->service), tmp, "");
            zrpc_copy_str(state->method_name, sizeof(state->method_name), dot + 1, "");
        }
    }
    if (!state || !state->call || !method) {
        return;
    }
    state->last_ms = zrpc_now_ms();
    state->bytes += payload ? payload->len : 0;
    request.service = state->service;
    request.method = state->method_name;
    request.payload = payload ? *payload : (zrpc_payload_t){0};
    request.peer_ip = state->call->peer_ip;
    request.peer_port = port;
    chunk.data = payload ? payload->data : NULL;
    chunk.len = payload ? payload->len : 0;
    chunk.encoding = payload ? payload->encoding : 0;
    chunk.offset = stream_meta->offset;
    chunk.flags = stream_meta->flags;
    method->stream_fn(state->call, &request, &chunk, method->user);

    if (stream_meta->flags & ZRPC_STREAM_LAST) {
        zrpc_mem_free(state->call);
        memset(state, 0, sizeof(*state));
    }
}

void zrpc_service_expire_streams(zrpc_node_t *node, uint64_t now_ms) {
    int i;
    if (!node) {
        return;
    }
    for (i = 0; i < ZRPC_MAX_RECV_STREAMS; i++) {
        zrpc_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && now_ms - stream->last_ms > ZRPC_STREAM_IDLE_TIMEOUT_MS) {
            zrpc_mem_free(stream->call);
            memset(stream, 0, sizeof(*stream));
        }
    }
}

int zrpc_service_create(zrpc_node_t *node, const char *service_name, zrpc_service_t **out) {
    zrpc_service_t *svc;
    zrpc_svc_task_t *t;

    if (!node || !service_name || !service_name[0] || !out) {
        return ZRPC_ERR_INVALID;
    }
    *out = NULL;
    svc = (zrpc_service_t *)calloc(1, sizeof(*svc));
    if (!svc) {
        return ZRPC_ERR_NOMEM;
    }
    svc->node = node;
    zrpc_copy_str(svc->name, sizeof(svc->name), service_name, "svc");
    t = (zrpc_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(svc);
        return ZRPC_ERR_NOMEM;
    }
    t->node = node;
    t->svc = svc;
    if (ztk_poller_async(node->poller, svc_link_task, t, 1) != ZTK_OK) {
        free(t);
        free(svc);
        return ZRPC_ERR_IO;
    }
    *out = svc;
    return ZRPC_OK;
}

void zrpc_service_destroy(zrpc_service_t *svc) {
    /* v1：不在运行期注销；节点销毁时统一释放。 */
    (void)svc;
}

static int service_add_method_ex(zrpc_service_t *svc, const char *method, zrpc_handler_fn fn,
                                 zrpc_chunk_handler_fn stream_fn, void *user, int owns_user) {
    zrpc_method_t *m;
    zrpc_svc_task_t *t;

    if (!svc || !method || !method[0] || (!fn && !stream_fn)) {
        return ZRPC_ERR_INVALID;
    }
    m = (zrpc_method_t *)calloc(1, sizeof(*m));
    if (!m) {
        return ZRPC_ERR_NOMEM;
    }
    zrpc_copy_str(m->name, sizeof(m->name), method, "m");
    m->fn = fn;
    m->stream_fn = stream_fn;
    m->user = user;
    m->owns_user = owns_user;
    t = (zrpc_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(m);
        return ZRPC_ERR_NOMEM;
    }
    t->node = svc->node;
    t->svc = svc;
    t->method = m;
    if (ztk_poller_async(svc->node->poller, svc_method_task, t, 1) != ZTK_OK) {
        free(t);
        free(m);
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

int zrpc_service_add_method(zrpc_service_t *svc, const char *method, zrpc_handler_fn fn, void *user) {
    return service_add_method_ex(svc, method, fn, NULL, user, 0);
}

int zrpc_service_add_sync_method(zrpc_service_t *svc, const char *method, zrpc_sync_handler_fn fn,
                                 void *user) {
    zrpc_sync_bridge_t *bridge;
    int rc;
    if (!svc || !method || !method[0] || !fn) {
        return ZRPC_ERR_INVALID;
    }
    bridge = (zrpc_sync_bridge_t *)calloc(1, sizeof(*bridge));
    if (!bridge) {
        return ZRPC_ERR_NOMEM;
    }
    bridge->fn = fn;
    bridge->user = user;
    rc = service_add_method_ex(svc, method, sync_method_bridge, NULL, bridge, 1);
    if (rc != ZRPC_OK) {
        free(bridge);
    }
    return rc;
}

int zrpc_service_add_stream_method(zrpc_service_t *svc, const char *method,
                                   zrpc_chunk_handler_fn fn, void *user) {
    return service_add_method_ex(svc, method, NULL, fn, user, 0);
}
