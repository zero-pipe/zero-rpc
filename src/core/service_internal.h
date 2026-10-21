#ifndef SERVICE_INTERNAL_H
#define SERVICE_INTERNAL_H

/* 服务端内部结构：方法表、服务对象、调用上下文、流接收状态。 */

#include <service.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "transport_internal.h"

typedef struct zri_method {
    struct zri_method *next;
    char name[METHOD_NAME_MAX];
    handler_fn fn;
    chunk_handler_fn stream_fn;
    void *user;
    int owns_user;                      /* user 由框架分配（如 sync 桥接结构），销毁时释放 */
    int run_in_worker;                  /* 非 0 时 unary handler 调度到 node->handler_pool */
    const service_executor_t *executor; /* 非 NULL 时使用调用者提供的 executor */
} zri_method_t;

struct service {
    struct service *next;
    node_t *node;
    char name[SERVICE_NAME_MAX];
    zri_method_t *methods;
    service_interceptor_fn interceptor;
    void *interceptor_user;
};

struct call {
    node_t *node;
    zri_transport_t *transport;
    ntk_poller *poller; /* 请求到达时的 poller；跨线程 reply 回投到这里 */
    zri_peer_token_t token;
    char peer_ip[ENDPOINT_MAX];
    uint16_t peer_port;
    uint32_t msg_id;
    uint64_t deadline_ms; /* 请求 deadline（绝对单调 ms，0 = 无） */
    int replied;
    int one_way; /* EVENT：handler 可复用，但不产生响应 */
    int stream_owned;
    int from_worker_pool; /* call 来自节点的 worker_call_pool */
};

/*
 * 在途调用的取消登记表（best-effort，对齐 gRPC cancellation）。客户端发送
 * KIND_CANCEL 后，服务端按 request_id + 对端登记；worker 任务启动前命中即跳过，
 * 运行中的 handler 可用 call_canceled() 查询。登记项由节点定时器按 TTL 过期。
 */
#define CANCEL_SLOTS 256
#define CANCEL_TTL_MS 30000
typedef struct zri_cancel_slot {
    int used;
    uint32_t request_id;
    char peer_ip[ENDPOINT_MAX];
    uint16_t peer_port;
    uint64_t expires_ms;
} zri_cancel_slot_t;

typedef struct zri_recv_stream {
    int used;
    uint32_t request_id;
    char peer_ip[ENDPOINT_MAX];
    uint16_t peer_port;
    call_t *call;
    struct zri_method *method;
    char service[SERVICE_NAME_MAX];
    char method_name[METHOD_NAME_MAX];
    uint64_t last_ms;
    uint64_t bytes;
    int run_in_worker;
    const service_executor_t *executor;
    int in_flight;
    int closing;
    int canceled;
    int intercepted;
    service_interceptor_fn interceptor;
    void *interceptor_user;
    metadata_t *metadata; /* 请求 metadata 解码副本，按需分配；无则为 NULL */
    uint64_t deadline_ms;
    struct zri_stream_task *pending_head;
    struct zri_stream_task *pending_tail;
} zri_recv_stream_t;

void zri_service_handle_request(node_t *node,
                                zri_transport_t *transport,
                                const zri_message_t *message);
void zri_service_handle_cancel(node_t *node,
                               zri_transport_t *transport,
                               const zri_message_t *message);
void zri_service_handle_stream_chunk(node_t *node,
                                     zri_transport_t *transport,
                                     const zri_message_t *message);
void zri_service_expire_streams(node_t *node, uint64_t now_ms);

/* 取消登记表：登记 / 查询 / 过期。查询不加锁地只读快照，避免热路径阻塞。 */
void zri_node_cancel_record(node_t *node,
                            uint32_t request_id,
                            const char *peer_ip,
                            uint16_t peer_port);
int zri_node_is_canceled(const node_t *node,
                         uint32_t request_id,
                         const char *peer_ip,
                         uint16_t peer_port);
void zri_node_expire_cancels(node_t *node, uint64_t now_ms);

/* worker 路径的 call 分配/释放：优先复用节点池，否则回退 malloc/zri_mem_free。 */
call_t *zri_worker_call_alloc(node_t *node);
void zri_worker_call_free(node_t *node, call_t *call);

/* reply.c：错误状态回包（路由/分配失败时直接发送，无 call 上下文）。 */
void zri_service_send_status(zri_transport_t *transport, const zri_message_t *request, int status);

#endif /* SERVICE_INTERNAL_H */
