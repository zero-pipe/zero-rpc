/*
 * 消息分发与出站传输选择。
 *
 * 从 node.c 拆出：入站把传输上抛的完整消息分派到 service（请求/事件）或
 * pending（响应）；出站按 scheme/transport 选中一条传输发送。
 */
#include "node_internal.h"

#include "envelope.h"
#include "atomic.h"
#include "pending.h"
#include "service_internal.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* 端到端延迟直方图上界（毫秒），与 metrics.h 的 LATENCY_HIST_BUCKETS 对应。 */
static const uint32_t g_latency_bounds[LATENCY_HIST_BUCKETS - 1] = {
    1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000
};

/* 记录一次已完成 RPC 的端到端延迟（从发起 pending 到收到响应）。 */
static void pending_record_latency(node_t *node, const zri_pending_t *p)
{
    uint64_t now;
    uint64_t ms;
    int bucket;
    if (!node || !p || p->started_ms == 0) {
        return;
    }
    now = zri_now_ms();
    ms = now > p->started_ms ? now - p->started_ms : 0;
    for (bucket = 0; bucket < LATENCY_HIST_BUCKETS - 1; bucket++) {
        if (ms <= g_latency_bounds[bucket]) {
            break;
        }
    }
    ZRI_METRIC_ADD(node->metrics.latency_hist[bucket], 1u);
    ZRI_METRIC_INC(node->metrics.latency_count);
    ZRI_METRIC_ADD(node->metrics.latency_sum_ms, ms);
}

/* 把响应帧上的 metadata / status message 解析到 pending（按需分配）。 */
static void pending_attach_response(zri_pending_t *p, const zri_message_t *msg)
{
    if (!p) {
        return;
    }
    if (p->response_metadata) {
        free(p->response_metadata);
        p->response_metadata = NULL;
    }
    if (p->response_message) {
        free(p->response_message);
        p->response_message = NULL;
    }
    if (msg->metadata_len > 0 && msg->metadata) {
        metadata_t *md = (metadata_t *)calloc(1, sizeof(*md));
        if (md) {
            if (metadata_decode(md, msg->metadata, msg->metadata_len) == OK && md->count > 0) {
                p->response_metadata = md;
            } else {
                free(md);
            }
        }
    }
    if (msg->status_msg_len > 0 && msg->status_msg) {
        p->response_message = (char *)malloc((size_t)msg->status_msg_len + 1);
        if (p->response_message) {
            memcpy(p->response_message, msg->status_msg, msg->status_msg_len);
            p->response_message[msg->status_msg_len] = '\0';
        }
    }
}

int zri_node_resolve(node_t *node, const char *service, int prefer_stream, stub_target_t *out)
{
    transport_kind_t kind;
    endpoint_t endpoint;

    if (!node || !service || !out) {
        return ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (zri_route_lookup_prefer(&node->routes,
                                service,
                                prefer_stream,
                                out->ip,
                                sizeof(out->ip),
                                &out->port,
                                &kind,
                                out->scheme,
                                sizeof(out->scheme)) == OK) {
        out->transport = kind;
        return OK;
    }
    if (zri_registry_select_prefer(&node->registry, service, prefer_stream, &endpoint) != OK) {
        return ERR_NOTFOUND;
    }
    zri_copy_str(out->ip, sizeof(out->ip), endpoint.host, "0.0.0.0");
    zri_copy_str(out->scheme, sizeof(out->scheme), endpoint.scheme, "udp");
    out->port = endpoint.port;
    out->transport = endpoint.transport;
    return OK;
}

/*
 * per-peer 入站限速：按 ip:port 做固定 1 秒窗口计数。未配置时零开销返回允许。
 * 命中上限返回 0，调用方对 REQUEST 回 ERR_AGAIN、对 EVENT 直接丢弃。
 */
static int node_rate_allow(node_t *node, const zri_message_t *msg)
{
    uint64_t now;
    int i;
    int free_slot = -1;
    if (!node->rate_limit_pps || !msg->peer_ip) {
        return 1;
    }
    now = zri_now_ms();
    ntk_mutex_lock(node->rate_lock);
    for (i = 0; i < RATE_LIMIT_SLOTS; i++) {
        zri_rate_slot_t *s = &node->rates[i];
        if (s->used && s->port == msg->peer_port && strcmp(s->ip, msg->peer_ip) == 0) {
            if (now - s->window_start_ms >= RATE_WINDOW_MS) {
                s->window_start_ms = now;
                s->count = 0;
            }
            if (s->count >= node->rate_limit_pps) {
                ntk_mutex_unlock(node->rate_lock);
                return 0;
            }
            s->count++;
            ntk_mutex_unlock(node->rate_lock);
            return 1;
        }
        if (!s->used || now - s->window_start_ms >= RATE_WINDOW_MS) {
            if (free_slot < 0) {
                free_slot = i;
            }
        }
    }
    if (free_slot < 0) {
        ntk_mutex_unlock(node->rate_lock);
        return 1; /* 槽位耗尽：退化为不限速，避免误伤新对端 */
    }
    {
        zri_rate_slot_t *s = &node->rates[free_slot];
        s->used = 1;
        zri_copy_str(s->ip, sizeof(s->ip), msg->peer_ip, "0.0.0.0");
        s->port = msg->peer_port;
        s->window_start_ms = now;
        s->count = 1;
    }
    ntk_mutex_unlock(node->rate_lock);
    return 1;
}

static void node_dispatch(node_t *node, zri_transport_t *transport, const zri_message_t *msg);

/*
 * 跨 poller 响应回投：pending 归属 poller 才处理本请求的超时/回调，因此响应
 * 若落在其他 poller，需要把消息字段深拷贝到任务，再 async 回归属 poller。
 */
typedef struct zri_response_task {
    node_t *node;
    zri_transport_t *transport;
    zri_message_t msg;
    char *route;
    char *peer_ip;
    uint8_t *metadata;
    uint8_t *status_msg;
    uint8_t *payload;
} zri_response_task_t;

static void response_task_destroy(zri_response_task_t *t)
{
    if (!t) {
        return;
    }
    free(t->route);
    free(t->peer_ip);
    free(t->metadata);
    free(t->status_msg);
    free(t->payload);
    free(t);
}

static int response_task_init(zri_response_task_t *t,
                              node_t *node,
                              zri_transport_t *transport,
                              const zri_message_t *msg)
{
    size_t route_len = msg->route ? strlen(msg->route) + 1 : 0;
    size_t ip_len = msg->peer_ip ? strlen(msg->peer_ip) + 1 : 0;

    memset(t, 0, sizeof(*t));
    t->node = node;
    t->transport = transport;
    t->msg = *msg;
    t->msg.peer_token = NULL;
    if (route_len) {
        t->route = (char *)malloc(route_len);
        if (!t->route) {
            goto fail;
        }
        memcpy(t->route, msg->route, route_len);
        t->msg.route = t->route;
    } else {
        t->msg.route = NULL;
    }
    if (ip_len) {
        t->peer_ip = (char *)malloc(ip_len);
        if (!t->peer_ip) {
            goto fail;
        }
        memcpy(t->peer_ip, msg->peer_ip, ip_len);
        t->msg.peer_ip = t->peer_ip;
    } else {
        t->msg.peer_ip = NULL;
    }
    if (msg->metadata_len > 0 && msg->metadata) {
        t->metadata = (uint8_t *)malloc(msg->metadata_len);
        if (!t->metadata) {
            goto fail;
        }
        memcpy(t->metadata, msg->metadata, msg->metadata_len);
        t->msg.metadata = t->metadata;
    } else {
        t->msg.metadata = NULL;
        t->msg.metadata_len = 0;
    }
    if (msg->status_msg_len > 0 && msg->status_msg) {
        t->status_msg = (uint8_t *)malloc(msg->status_msg_len);
        if (!t->status_msg) {
            goto fail;
        }
        memcpy(t->status_msg, msg->status_msg, msg->status_msg_len);
        t->msg.status_msg = (const char *)t->status_msg;
    } else {
        t->msg.status_msg = NULL;
        t->msg.status_msg_len = 0;
    }
    if (msg->payload.len > 0 && msg->payload.data) {
        t->payload = (uint8_t *)malloc(msg->payload.len);
        if (!t->payload) {
            goto fail;
        }
        memcpy(t->payload, msg->payload.data, msg->payload.len);
        t->msg.payload.data = t->payload;
    } else {
        t->msg.payload.data = NULL;
        t->msg.payload.len = 0;
    }
    return OK;
fail:
    response_task_destroy(t);
    return ERR_NOMEM;
}

static void response_dispatch_task(void *user)
{
    zri_response_task_t *t = (zri_response_task_t *)user;
    node_dispatch(t->node, t->transport, &t->msg);
    response_task_destroy(t);
}

static void node_dispatch(node_t *node, zri_transport_t *transport, const zri_message_t *msg)
{
    if (msg->kind == KIND_CANCEL) {
        zri_service_handle_cancel(node, transport, msg);
        return;
    }
    if (msg->kind == KIND_REQUEST || msg->kind == KIND_EVENT) {
        if (!node_rate_allow(node, msg)) {
            ZRI_METRIC_INC(node->metrics.rate_limited);
            if (msg->kind == KIND_REQUEST) {
                zri_service_send_status(transport, msg, ERR_AGAIN);
            }
            return;
        }
        if (msg->has_stream) {
            zri_service_handle_stream_chunk(node, transport, msg);
        } else {
            zri_service_handle_request(node, transport, msg);
        }
        return;
    }
    if (msg->kind == KIND_RESPONSE) {
        ntk_poller *owner = zri_pending_owner(node, msg->request_id);
        if (owner && !ntk_poller_is_current_thread(owner)) {
            zri_response_task_t *task =
                (zri_response_task_t *)calloc(1, sizeof(*task));
            if (!task) {
                return;
            }
            if (response_task_init(task, node, transport, msg) != OK) {
                return; /* response_task_init 已释放 task */
            }
            if (ntk_poller_async(owner, response_dispatch_task, task, 1) != NTK_OK) {
                response_task_destroy(task);
            }
            return;
        }
        zri_pending_t *p = zri_pending_find(node, msg->request_id);
        response_t resp;
        if (!p) {
            return;
        }
        ZRI_METRIC_INC(node->metrics.responses_received);
        ZRI_METRIC_ADD(node->metrics.bytes_received, msg->payload.len);
        if (msg->flags & MSG_FLAG_FRAMEWORK_STATUS) {
            /* 框架错误走回调 status 通道，response_t.status 只放业务码。 */
            pending_record_latency(node, p);
            zri_pending_invoke(p, msg->status, NULL);
            return;
        }
        pending_attach_response(p, msg);
        if (p->streaming) {
            int app_status = msg->status;
            chunk_t chunk;
            chunk.data = msg->payload.data;
            chunk.len = msg->payload.len;
            chunk.encoding = msg->payload.encoding;
            chunk.offset = msg->has_stream ? msg->stream.offset : p->expected_offset;
            chunk.flags = msg->has_stream ? msg->stream.flags : 0;
            chunk.metadata = p->response_metadata;
            chunk.message = p->response_message;
            if (p->chunk_cb) {
                p->chunk_cb(app_status, &chunk, p->stream_user);
            }
            p->expected_offset = chunk.offset + chunk.len;
            if (chunk.flags & STREAM_LAST) {
                stream_done_fn done = p->done_cb;
                void *stream_user = p->stream_user;
                pending_record_latency(node, p);
                p->chunk_cb = NULL;
                p->done_cb = NULL;
                if (done) {
                    done(app_status, stream_user);
                }
                zri_pending_release(p);
            }
            return;
        }
        resp.status = msg->status;
        resp.message = p->response_message;
        resp.payload = msg->payload;
        resp.metadata = p->response_metadata;
        pending_record_latency(node, p);
        zri_pending_invoke(p, OK, &resp);
        return;
    }
    /* EVENT: 预留，忽略 */
}

void zri_node_on_message(node_t *node, zri_transport_t *transport, const zri_message_t *msg)
{
    if (!node || !transport || !msg) {
        return;
    }
    node_dispatch(node, transport, msg);
}

int zri_node_send(node_t *node,
                  transport_kind_t transport_kind,
                  const char *scheme,
                  const zri_message_t *msg)
{
    size_t i;
    if (!node || !msg || !msg->peer_ip || !msg->peer_port) {
        return ERR_INVALID;
    }
    for (i = 0; i < node->transport_count; i++) {
        zri_transport_t *transport = node->transports[i];
        if (!transport) {
            continue;
        }
        if (scheme && scheme[0] && strcmp(transport->scheme, scheme) != 0) {
            continue;
        }
        if ((!scheme || !scheme[0]) && transport->kind != transport_kind) {
            continue;
        }
        return zri_transport_send(transport, msg);
    }
    return ERR_NOTFOUND;
}

int zri_node_send_call(node_t *node,
                       const stub_target_t *target,
                       const char *service,
                       const char *method,
                       const payload_t *payload,
                       uint8_t kind,
                       uint64_t request_id,
                       uint64_t timeout_ms,
                       const void *metadata,
                       uint16_t metadata_len)
{
    zri_message_t message;
    char route[MAX_ROUTE];

    if (!node || !target || !service || !method || !target->ip[0] || !target->port) {
        return ERR_INVALID;
    }
    if (zri_route_format(route, sizeof(route), service, method) != OK) {
        return ERR_INVALID;
    }
    memset(&message, 0, sizeof(message));
    message.kind = kind;
    message.request_id = (uint32_t)request_id;
    /* 请求剩余超时透传给服务端（0 = 无 deadline）；CANCEL 不使用该字段。 */
    message.timeout_ms = timeout_ms > 0xffffffffull ? 0xffffffffu : (uint32_t)timeout_ms;
    message.metadata = metadata;
    message.metadata_len = metadata_len;
    message.route = route;
    message.payload = payload ? *payload : (payload_t){ 0 };
    message.peer_ip = target->ip;
    message.peer_port = target->port;
    return zri_node_send(node, target->transport, target->scheme, &message);
}
