#ifndef ZRPC_REGISTRY_H
#define ZRPC_REGISTRY_H

#include <zrpc/zrpc_endpoint.h>

#include <stddef.h>
#include <stdint.h>

typedef struct zrpc_registry_instance zrpc_registry_instance_t;

/*
 * 服务实例注册表：自身即全部状态（含锁），不依赖 node。
 * node 在组合根处内嵌一个 zrpc_registry_t 并把它传进来。
 */
typedef struct zrpc_registry {
    zrpc_registry_instance_t *instances;
    void *lock;
    uint64_t generation;
    size_t next_instance;
} zrpc_registry_t;

void zrpc_registry_init(zrpc_registry_t *registry);
void zrpc_registry_cleanup(zrpc_registry_t *registry);

int zrpc_registry_upsert(zrpc_registry_t *registry, const char *service, const char *instance_id,
                         const zrpc_endpoint_t *endpoints, size_t endpoint_count,
                         uint32_t node_id, uint64_t last_seen_ms);
void zrpc_registry_remove_node(zrpc_registry_t *registry, uint32_t node_id);
int zrpc_registry_replace_node(zrpc_registry_t *registry, uint32_t node_id, const char *instance_id,
                               const char *const *services, size_t service_count,
                               const zrpc_endpoint_t *endpoints, size_t endpoint_count,
                               uint64_t last_seen_ms);
void zrpc_registry_expire(zrpc_registry_t *registry, uint64_t now_ms, uint64_t ttl_ms);

/* Selector operation: returns one endpoint using registry-local round robin. */
int zrpc_registry_select(zrpc_registry_t *registry, const char *service, zrpc_endpoint_t *out);
/* Prefer a TCP endpoint when prefer_stream is set. */
int zrpc_registry_select_prefer(zrpc_registry_t *registry, const char *service, int prefer_stream,
                                zrpc_endpoint_t *out);

#endif /* ZRPC_REGISTRY_H */
