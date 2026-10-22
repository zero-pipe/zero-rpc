#ifndef NB_NODE_H
#define NB_NODE_H

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
int nb_node_create(const nb_node_config_t *cfg, nb_node_t **out);
void nb_node_destroy(nb_node_t *node);

/* ---- 节点状态与端点 ---- */
const char *nb_node_name(const nb_node_t *node);
nb_transport_kind_t nb_node_transport(const nb_node_t *node);
uint16_t nb_node_discovery_port(const nb_node_t *node);
uint16_t nb_node_data_port(const nb_node_t *node);
size_t nb_node_binding_count(const nb_node_t *node);
int nb_node_binding_endpoint(const nb_node_t *node, size_t index, char *out, size_t cap);

/* ---- 指标与对端质量 ---- */
void nb_node_metrics(const nb_node_t *node, nb_metrics_t *out);
/* 查询某对端（ip:port）的链路统计；无此对端返回 NB_ERR_NOTFOUND。 */
int nb_node_peer_stats(const nb_node_t *node, const char *ip, uint16_t port, nb_peer_stats_t *out);

/* ---- 发现与路由 ---- */
/* 显式加入一个发现对端（单播 HELLO 目标）；用于定向发现/测试。 */
int nb_node_add_discovery_peer(nb_node_t *node, const char *ip, uint16_t discovery_port);

/* 静态路由：直接登记 service -> endpoint（跳过发现）。 */
int nb_node_add_static_route(
    nb_node_t *node, const char *service, const char *ip, uint16_t port, nb_transport_kind_t transport);
int nb_node_add_static_endpoint(nb_node_t *node, const char *service, const char *endpoint);

/* ---- 服务等待与调度 ---- */
/* 等待目标服务出现在路由表（MESH 启动期用）。STATIC 立即返回 NB_OK。 */
int nb_node_wait_for_service(nb_node_t *node, const char *service, uint64_t timeout_ms);

/*
 * 在节点的 poller 线程投递任务（异步、不阻塞调用方）。
 * 高吞吐客户端可在回调/任务里直接发起下一次调用，nb_proxy_call_async 会识别
 * "已在 poller 线程"并内联执行，消除跨线程开销。
 */
int nb_node_post(nb_node_t *node, void (*fn)(void *user), void *user);
int nb_node_post_delay(nb_node_t *node, uint64_t delay_ms, void (*fn)(void *user), void *user);

/* ---- 阻塞运行 ---- */
/* 同上，但最多等待 timeout_ms，返回 NB_ERR_TIMEOUT。 */
int nb_node_spin_once(nb_node_t *node, uint64_t timeout_ms);
/* 阻塞运行直到进程收到 SIGINT/SIGTERM（用于服务端 main）。 */
int nb_node_spin(nb_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* NB_NODE_H */
