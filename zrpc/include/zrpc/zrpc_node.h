#ifndef ZRPC_NODE_H
#define ZRPC_NODE_H

/*
 * 节点：生命周期、传输选择、线程与发现配置。
 *
 * 一个节点持有：
 *   - 一个 ztk_poller_pool（IO 线程池，不创建自有线程）；
 *   - 一种传输（UDP 或 TCP，见 zrpc_node_config_t.transport）；
 *   - 可选的服务发现（MESH）与路由表。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_metrics.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_binding_config {
    const char *name;                  /* provider/binding name, e.g. "tcp" */
    const char *endpoint;              /* URI, e.g. tcp://127.0.0.1:8080 */
} zrpc_binding_config_t;

typedef struct zrpc_node_config {
    const char *name;                  /* 节点名（日志/发现），可空 */
    zrpc_transport_kind_t transport;   /* 数据面传输，默认 UDP */
    const char *bind_host;             /* 绑定地址，默认 "0.0.0.0" */
    uint16_t data_port;                /* 数据面端口（UDP/TCP），0 = 自动分配 */
    const char *discovery_host;        /* 发现目标地址，默认 "255.255.255.255" */
    uint16_t discovery_port;           /* 发现端口，0 = ZRPC_DISCOVERY_PORT */
    unsigned io_threads;               /* poller 线程数，0 = 硬件并发 */
    uint32_t hello_interval_ms;        /* 心跳间隔，0 = 3000 */
    uint32_t node_ttl_ms;              /* 对端过期时间，0 = 9000 */
    zrpc_node_mode_t mode;             /* 0 = MESH（默认），1 = STATIC */
    uint16_t frag_bytes;               /* UDP 每分片 payload 字节（0 = 1200），TCP 忽略 */
    uint32_t max_msg_bytes;            /* 单条 envelope 上限，0 = 默认 1 MiB */
    uint32_t backpressure_bytes;       /* 单 link 出站队列高水位，0 = 默认 4 MiB */

    /* New transport model. If non-empty, every binding is started. */
    const zrpc_binding_config_t *bindings;
    size_t binding_count;
} zrpc_node_config_t;

int zrpc_node_create(const zrpc_node_config_t *cfg, zrpc_node_t **out);
void zrpc_node_destroy(zrpc_node_t *node);

const char *zrpc_node_name(const zrpc_node_t *node);
zrpc_transport_kind_t zrpc_node_transport(const zrpc_node_t *node);
uint16_t zrpc_node_data_port(const zrpc_node_t *node);
uint16_t zrpc_node_discovery_port(const zrpc_node_t *node);
size_t zrpc_node_binding_count(const zrpc_node_t *node);
int zrpc_node_binding_endpoint(const zrpc_node_t *node, size_t index, char *out, size_t cap);
void zrpc_node_metrics(const zrpc_node_t *node, zrpc_metrics_t *out);

/* 显式加入一个发现对端（单播 HELLO 目标）；用于定向发现/测试。 */
int zrpc_node_add_discovery_peer(zrpc_node_t *node, const char *ip, uint16_t discovery_port);

/* 静态路由：直接登记 service -> endpoint（跳过发现）。 */
int zrpc_node_add_static_route(zrpc_node_t *node, const char *service, const char *ip,
                               uint16_t port, zrpc_transport_kind_t transport);
int zrpc_node_add_static_endpoint(zrpc_node_t *node, const char *service, const char *endpoint);

/*
 * 在节点的 poller 线程投递任务（异步、不阻塞调用方）。
 * 高吞吐客户端可在回调/任务里直接发起下一次调用，zrpc_proxy_call 会识别
 * "已在 poller 线程"并内联执行，消除跨线程开销。
 */
int zrpc_node_post(zrpc_node_t *node, void (*fn)(void *user), void *user);
int zrpc_node_post_delay(zrpc_node_t *node, uint64_t delay_ms, void (*fn)(void *user), void *user);

/* 等待目标服务出现在路由表（MESH 启动期用）。STATIC 立即返回 OK。 */
int zrpc_node_wait_for_service(zrpc_node_t *node, const char *service, uint64_t timeout_ms);

/* 阻塞运行直到进程收到 SIGINT/SIGTERM（用于服务端 main）。 */
int zrpc_node_spin(zrpc_node_t *node);
/* 同上，但最多等待 timeout_ms，返回 ZRPC_ERR_TIMEOUT。 */
int zrpc_node_spin_once(zrpc_node_t *node, uint64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_NODE_H */
