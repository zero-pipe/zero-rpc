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
#include <config.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
int node_peer_stats(const node_t *node, const char *ip, uint16_t port, peer_stats_t *out);

/* ---- 发现与路由 ---- */
/* 显式加入一个发现对端（单播 HELLO 目标）；用于定向发现/测试。 */
int node_add_discovery_peer(node_t *node, const char *ip, uint16_t discovery_port);

/* 静态路由：直接登记 service -> endpoint（跳过发现）。 */
int node_add_static_route(
    node_t *node, const char *service, const char *ip, uint16_t port, transport_kind_t transport);
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
