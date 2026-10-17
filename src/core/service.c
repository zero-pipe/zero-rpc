/* 服务端：方法注册与请求分发（回复见 reply.c）。 */
#include "node_internal.h"
#include "service_internal.h"

#include "envelope.h"
#include "mem.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* 同步风格 handler 的桥接：返回值即状态码，把视图回复后再释放。 */
typedef struct zri_sync_bridge {
    sync_handler_fn fn;
    void *user;
} zri_sync_bridge_t;

static void sync_method_bridge(call_t *call, const request_t *req, void *user) {
    zri_sync_bridge_t *bridge = (zri_sync_bridge_t *)user;
    message_view_t view;
    payload_t pl;
    int status;
    message_view_init(&view);
    status = bridge->fn(req, &view, bridge->user);
    message_view_to_payload(&view, &pl);
    reply(call, status, &pl);
    message_view_release(&view);
}

typedef struct zri_svc_task {
    node_t *node;
    service_t *svc;
    zri_method_t *method;
} zri_svc_task_t;

static void svc_link_task(void *user) {
    zri_svc_task_t *t = (zri_svc_task_t *)user;
    t->svc->next = t->node->services;
    t->node->services = t->svc;
    if (t->node->registry_backend && t->node->registry_backend->ops->announce) {
        t->node->registry_backend->ops->announce(t->node, 0);
    }
    free(t);
}

static void svc_method_task(void *user) {
    zri_svc_task_t *t = (zri_svc_task_t *)user;
    t->method->next = t->svc->methods;
    t->svc->methods = t->method;
    free(t);
}

void zri_service_handle_request(node_t *node, zri_transport_t *transport,
                                const zri_message_t *msg) {
    char tmp[MAX_ROUTE];
    char *dot;
    service_t *svc;
    zri_method_t *m;
    handler_fn fn = NULL;
    void *user = NULL;

    if (!node || !msg) {
        return;
    }
    zri_copy_str(tmp, sizeof(tmp), msg->route, "");
    dot = strchr(tmp, '.');
    if (!dot) {
        zri_service_send_status(transport, msg, ERR_NOTFOUND);
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
        zri_service_send_status(transport, msg, ERR_NOTFOUND);
        return;
    }
    {
        call_t *call = (call_t *)zri_mem_alloc(sizeof(*call));
        request_t req;
        if (!call) {
            zri_service_send_status(transport, msg, ERR_NOMEM);
            return;
        }
        memset(call, 0, sizeof(*call));
        call->node = node;
        call->transport = transport;
        call->token = msg->peer_token;
        zri_copy_str(call->peer_ip, sizeof(call->peer_ip), msg->peer_ip, "0.0.0.0");
        call->peer_port = msg->peer_port;
        call->msg_id = msg->request_id;
        call->one_way = msg->kind == KIND_EVENT;
        req.service = tmp;
        req.method = dot + 1;
        req.payload = msg->payload;
        req.peer_ip = call->peer_ip;
        req.peer_port = msg->peer_port;
        fn(call, &req, user);
        if (call->one_way) {
            zri_mem_free(call);
        }
    }
}

static zri_recv_stream_t *find_recv_stream(node_t *node, const zri_message_t *msg) {
    int i;
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        zri_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && stream->request_id == msg->request_id &&
            stream->peer_port == msg->peer_port &&
            strcmp(stream->peer_ip, msg->peer_ip ? msg->peer_ip : "") == 0) {
            return stream;
        }
    }
    return NULL;
}

static zri_recv_stream_t *alloc_recv_stream(node_t *node, zri_transport_t *transport,
                                             const zri_message_t *msg) {
    int i;
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        zri_recv_stream_t *stream = &node->recv_streams[i];
        if (!stream->used) {
            call_t *call = (call_t *)zri_mem_alloc(sizeof(*call));
            if (!call) {
                return NULL;
            }
            memset(call, 0, sizeof(*call));
            stream->used = 1;
            stream->request_id = msg->request_id;
            zri_copy_str(stream->peer_ip, sizeof(stream->peer_ip), msg->peer_ip, "0.0.0.0");
            stream->peer_port = msg->peer_port;
            stream->call = call;
            stream->last_ms = zri_now_ms();
            stream->bytes = 0;
            call->node = node;
            call->transport = transport;
            call->token = msg->peer_token;
            zri_copy_str(call->peer_ip, sizeof(call->peer_ip), msg->peer_ip, "0.0.0.0");
            call->peer_port = msg->peer_port;
            call->msg_id = msg->request_id;
            call->stream_owned = 1;
            return stream;
        }
    }
    return NULL;
}

void zri_service_handle_stream_chunk(node_t *node, zri_transport_t *transport,
                                     const zri_message_t *msg) {
    char tmp[MAX_ROUTE];
    char *dot;
    service_t *svc;
    zri_method_t *method = NULL;
    zri_recv_stream_t *state;
    request_t request;
    chunk_t chunk;
    const stream_meta_t *stream_meta;

    if (!node || !msg || !msg->has_stream) {
        return;
    }
    stream_meta = &msg->stream;
    state = find_recv_stream(node, msg);

    if (stream_meta->flags & STREAM_CANCEL) {
        if (state) {
            zri_mem_free(state->call);
            memset(state, 0, sizeof(*state));
        }
        return;
    }

    if (state) {
        method = state->method;
    } else {
        if (!(stream_meta->flags & STREAM_FIRST)) {
            return; /* 没有 FIRST 也没有已有状态：协议错误 */
        }
        zri_copy_str(tmp, sizeof(tmp), msg->route, "");
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
        state = alloc_recv_stream(node, transport, msg);
        if (state) {
            state->method = method;
            zri_copy_str(state->service, sizeof(state->service), tmp, "");
            zri_copy_str(state->method_name, sizeof(state->method_name), dot + 1, "");
        }
    }
    if (!state || !state->call || !method) {
        return;
    }
    state->last_ms = zri_now_ms();
    state->bytes += msg->payload.len;
    request.service = state->service;
    request.method = state->method_name;
    request.payload = msg->payload;
    request.peer_ip = state->call->peer_ip;
    request.peer_port = msg->peer_port;
    chunk.data = msg->payload.data;
    chunk.len = msg->payload.len;
    chunk.encoding = msg->payload.encoding;
    chunk.offset = stream_meta->offset;
    chunk.flags = stream_meta->flags;
    method->stream_fn(state->call, &request, &chunk, method->user);

    if (stream_meta->flags & STREAM_LAST) {
        zri_mem_free(state->call);
        memset(state, 0, sizeof(*state));
    }
}

void zri_service_expire_streams(node_t *node, uint64_t now_ms) {
    int i;
    if (!node) {
        return;
    }
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        zri_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && now_ms - stream->last_ms > STREAM_IDLE_TIMEOUT_MS) {
            zri_mem_free(stream->call);
            memset(stream, 0, sizeof(*stream));
        }
    }
}

/* ---- 服务生命周期 ---- */

int service_create(node_t *node, const char *service_name, service_t **out) {
    service_t *svc;
    zri_svc_task_t *t;

    if (!node || !service_name || !service_name[0] || !out) {
        return ERR_INVALID;
    }
    *out = NULL;
    svc = (service_t *)calloc(1, sizeof(*svc));
    if (!svc) {
        return ERR_NOMEM;
    }
    svc->node = node;
    zri_copy_str(svc->name, sizeof(svc->name), service_name, "svc");
    t = (zri_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(svc);
        return ERR_NOMEM;
    }
    t->node = node;
    t->svc = svc;
    if (ntk_poller_async(node->poller, svc_link_task, t, 1) != NTK_OK) {
        free(t);
        free(svc);
        return ERR_IO;
    }
    *out = svc;
    return OK;
}

void service_destroy(service_t *svc) {
    /* v1：不在运行期注销；节点销毁时统一释放。 */
    (void)svc;
}

/* ---- 方法注册 ---- */

static int service_add_method_ex(service_t *svc, const char *method, handler_fn fn,
                                 chunk_handler_fn stream_fn, void *user, int owns_user) {
    zri_method_t *m;
    zri_svc_task_t *t;

    if (!svc || !method || !method[0] || (!fn && !stream_fn)) {
        return ERR_INVALID;
    }
    m = (zri_method_t *)calloc(1, sizeof(*m));
    if (!m) {
        return ERR_NOMEM;
    }
    zri_copy_str(m->name, sizeof(m->name), method, "m");
    m->fn = fn;
    m->stream_fn = stream_fn;
    m->user = user;
    m->owns_user = owns_user;
    t = (zri_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(m);
        return ERR_NOMEM;
    }
    t->node = svc->node;
    t->svc = svc;
    t->method = m;
    if (ntk_poller_async(svc->node->poller, svc_method_task, t, 1) != NTK_OK) {
        free(t);
        free(m);
        return ERR_IO;
    }
    return OK;
}

int service_add_method(service_t *svc, const char *method, handler_fn fn, void *user) {
    return service_add_method_ex(svc, method, fn, NULL, user, 0);
}

int service_add_sync_method(service_t *svc, const char *method, sync_handler_fn fn,
                                 void *user) {
    zri_sync_bridge_t *bridge;
    int rc;
    if (!svc || !method || !method[0] || !fn) {
        return ERR_INVALID;
    }
    bridge = (zri_sync_bridge_t *)calloc(1, sizeof(*bridge));
    if (!bridge) {
        return ERR_NOMEM;
    }
    bridge->fn = fn;
    bridge->user = user;
    rc = service_add_method_ex(svc, method, sync_method_bridge, NULL, bridge, 1);
    if (rc != OK) {
        free(bridge);
    }
    return rc;
}

int service_add_stream_method(service_t *svc, const char *method,
                                   chunk_handler_fn fn, void *user) {
    return service_add_method_ex(svc, method, NULL, fn, user, 0);
}
