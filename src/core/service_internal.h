#ifndef SERVICE_INTERNAL_H
#define SERVICE_INTERNAL_H

/* 服务端内部结构：方法表、服务对象、调用上下文、流接收状态。 */

#include <service.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "transport_internal.h"

typedef struct nb_method {
    struct nb_method *next;
    char name[NB_METHOD_NAME_MAX];
    handler_fn fn;
    chunk_handler_fn stream_fn;
    void *user;
    int owns_user;                      /* user 由框架分配（如 sync 桥接结构），销毁时释放 */
    int run_in_worker;                  /* 非 0 时 unary handler 调度到 node->handler_pool */
    const nb_service_executor_t *executor; /* 非 NULL 时使用调用者提供的 executor */
} nb_method_t;

struct nb_service {
    struct nb_service *next;
    nb_node_t *node;
    char name[NB_SERVICE_NAME_MAX];
    nb_method_t *methods;
    service_interceptor_fn interceptor;
    void *interceptor_user;
    int removed; /* nb_service_destroy 后置位：不再匹配新请求，内存留到 node 销毁 */
};

struct nb_call {
    nb_node_t *node;
    nb_transport_t *transport;
    ntk_poller *poller; /* 请求到达时的 poller；跨线程 nb_reply 回投到这里 */
    nb_peer_token_t token;
    char peer_ip[NB_ENDPOINT_MAX];
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
 * 运行中的 handler 可用 nb_call_canceled() 查询。登记项由节点定时器按 TTL 过期。
 */
#define CANCEL_SLOTS 256
#define CANCEL_TTL_MS 30000
typedef struct nb_cancel_slot {
    int used;
    uint32_t request_id;
    char peer_ip[NB_ENDPOINT_MAX];
    uint16_t peer_port;
    uint64_t expires_ms;
} nb_cancel_slot_t;

typedef struct nb_recv_stream {
    int used;
    uint32_t request_id;
    char peer_ip[NB_ENDPOINT_MAX];
    uint16_t peer_port;
    nb_call_t *call;
    struct nb_method *method;
    char service[NB_SERVICE_NAME_MAX];
    char method_name[NB_METHOD_NAME_MAX];
    uint64_t last_ms;
    uint64_t bytes;
    int run_in_worker;
    const nb_service_executor_t *executor;
    int in_flight;
    int closing;
    int canceled;
    int intercepted;
    service_interceptor_fn interceptor;
    void *interceptor_user;
    nb_metadata_t *metadata; /* 请求 metadata 解码副本，按需分配；无则为 NULL */
    uint64_t deadline_ms;
    struct nb_stream_task *pending_head;
    struct nb_stream_task *pending_tail;
} nb_recv_stream_t;

void nb_service_handle_request(nb_node_t *node,
                                nb_transport_t *transport,
                                const nb_message_t *message);
void nb_service_handle_cancel(nb_node_t *node,
                               nb_transport_t *transport,
                               const nb_message_t *message);
void nb_service_handle_stream_chunk(nb_node_t *node,
                                     nb_transport_t *transport,
                                     const nb_message_t *message);
void nb_service_expire_streams(nb_node_t *node, uint64_t now_ms);

/* 取消登记表：登记 / 查询 / 过期。查询不加锁地只读快照，避免热路径阻塞。 */
void nb_node_cancel_record(nb_node_t *node,
                            uint32_t request_id,
                            const char *peer_ip,
                            uint16_t peer_port);
int nb_node_is_canceled(const nb_node_t *node,
                         uint32_t request_id,
                         const char *peer_ip,
                         uint16_t peer_port);
void nb_node_expire_cancels(nb_node_t *node, uint64_t now_ms);

/* worker 路径的 call 分配/释放：优先复用节点池，否则回退 malloc/nb_mem_free。 */
nb_call_t *nb_worker_call_alloc(nb_node_t *node);
void nb_worker_call_free(nb_node_t *node, nb_call_t *call);

/* nb_reply.c：错误状态回包（路由/分配失败时直接发送，无 call 上下文）。 */
void nb_service_send_status(nb_transport_t *transport, const nb_message_t *request, int status);

#endif /* SERVICE_INTERNAL_H */
