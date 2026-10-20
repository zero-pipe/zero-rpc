#ifndef NODE_H
#define NODE_H

/*
 * 节点：生命周期、传输选择、线程与发现配置。
 *
 * 一个节点持有：
 *   - 一个 ntk_poller_pool（IO 线程池，不创建自有线程）；
 *   - 一个或多个 binding；未显式配置 bindings 时使用 transport 配置创建一个 binding；
 *   - 可选的服务发现（MESH）与路由表。
 */

#include <types.h>
#include <metrics.h>
#include <service_table.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct binding_config {
    const char *name;                  /* provider/binding name, e.g. "tcp" */
    const char *endpoint;              /* URI, e.g. tcp://127.0.0.1:8080 */
} binding_config_t;

typedef struct node_config {
    const char *name;                  /* 节点名（日志/发现），可空 */
    transport_kind_t transport;   /* 数据面传输，默认 UDP */
    const char *bind_host;             /* 绑定地址，默认 "0.0.0.0" */
    uint16_t data_port;                /* 数据面端口（UDP/TCP），0 = 自动分配 */
    const char *discovery_host;        /* 发现目标地址，默认 "255.255.255.255" */
    uint16_t discovery_port;           /* 发现端口，0 = DISCOVERY_PORT */
    unsigned io_threads;               /* poller 线程数，0 = 硬件并发 */
    /*
     * 业务 handler 线程池大小：0 = 关闭（handler 在 poller 线程内联执行）。
     * >0 时创建 ntk_thread_pool，配合 service_method_options_t 的
     * SERVICE_EXEC_WORKER（或 *_worker 便捷接口）把可能阻塞的 handler 移出
     * poller，避免单个慢 handler 卡死整条 poller。回复会自动回投原 poller
     * 发送；未配置时 WORKER 方法退化为内联执行并告警。
     */
    unsigned handler_threads;
    /*
     * worker 未完成任务上限（排队 + 执行中）：0 = 不限制（默认）。
     * >0 时超出上限的新请求不再排队，直接回 ERR_AGAIN（对齐 brpc 的
     * max_concurrency/ELIMIT），避免无界排队导致内存与尾延迟失控。
     */
    unsigned handler_max_concurrency;
    uint32_t hello_interval_ms;        /* 心跳间隔，0 = 3000 */
    uint32_t node_ttl_ms;              /* 对端过期时间，0 = 9000 */
    node_mode_t mode;             /* 0 = MESH（默认），1 = STATIC */
    uint16_t frag_bytes;               /* UDP 每分片 payload 字节（0 = 1200），TCP 忽略 */
    uint32_t max_msg_bytes;            /* 单条 envelope 上限，0 = 默认 1 MiB */
    uint32_t backpressure_bytes;       /* 单 link 出站队列高水位，0 = 默认 4 MiB */
    uint32_t rtcp_interval_ms;         /* UDP RTCP SR/RR 上报间隔，0 = 默认 1000ms */
    int rtcp_disabled;                 /* 非 0 关闭 RTCP 报告 */

    /* New transport model. If non-empty, every binding is started. */
    const binding_config_t *bindings;
    size_t binding_count;

    /*
     * Optional shared service declaration table (see service_table.h).
     * When non-NULL it is installed by node_create(); the caller must keep
     * it alive (use SERVICE_DEFINE so it has static storage).
     */
    const service_table_t *service_table;
} node_config_t;

/* ---- 生命周期 ---- */
int node_create(const node_config_t *cfg, node_t **out);
void node_destroy(node_t *node);

/* ---- 节点状态与端点 ---- */
const char *node_name(const node_t *node);
transport_kind_t node_transport(const node_t *node);
uint16_t node_discovery_port(const node_t *node);
uint16_t node_data_port(const node_t *node);
size_t node_binding_count(const node_t *node);
int node_binding_endpoint(const node_t *node, size_t index, char *out, size_t cap);

/* ---- 指标与对端质量 ---- */
void node_metrics(const node_t *node, metrics_t *out);
/* 查询某对端（ip:port）的链路统计；无此对端返回 ERR_NOTFOUND。 */
int node_peer_stats(const node_t *node, const char *ip, uint16_t port,
                         peer_stats_t *out);

/* ---- 发现与路由 ---- */
/* 显式加入一个发现对端（单播 HELLO 目标）；用于定向发现/测试。 */
int node_add_discovery_peer(node_t *node, const char *ip, uint16_t discovery_port);

/* 静态路由：直接登记 service -> endpoint（跳过发现）。 */
int node_add_static_route(node_t *node, const char *service, const char *ip,
                               uint16_t port, transport_kind_t transport);
int node_add_static_endpoint(node_t *node, const char *service, const char *endpoint);

/* ---- 服务等待与调度 ---- */
/* 等待目标服务出现在路由表（MESH 启动期用）。STATIC 立即返回 OK。 */
int node_wait_for_service(node_t *node, const char *service, uint64_t timeout_ms);

/*
 * 在节点的 poller 线程投递任务（异步、不阻塞调用方）。
 * 高吞吐客户端可在回调/任务里直接发起下一次调用，proxy_call_async 会识别
 * "已在 poller 线程"并内联执行，消除跨线程开销。
 */
int node_post(node_t *node, void (*fn)(void *user), void *user);
int node_post_delay(node_t *node, uint64_t delay_ms, void (*fn)(void *user), void *user);

/* ---- 阻塞运行 ---- */
/* 同上，但最多等待 timeout_ms，返回 ERR_TIMEOUT。 */
int node_spin_once(node_t *node, uint64_t timeout_ms);
/* 阻塞运行直到进程收到 SIGINT/SIGTERM（用于服务端 main）。 */
int node_spin(node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* NODE_H */
