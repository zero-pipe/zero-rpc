#ifndef ZRPC_REGISTRY_H
#define ZRPC_REGISTRY_H

#include <zrpc/zrpc_endpoint.h>

#include <stddef.h>
#include <stdint.h>

typedef struct zri_registry_instance zri_registry_instance_t;

/*
 * 服务实例注册表：自身即全部状态（含锁），不依赖 node。
 * node 在组合根处内嵌一个 zri_registry_t 并把它传进来。
 */
typedef struct zri_registry {
    zri_registry_instance_t *instances;
    void *lock;
    uint64_t generation;
    size_t next_instance;
} zri_registry_t;

void zri_registry_init(zri_registry_t *registry);
void zri_registry_cleanup(zri_registry_t *registry);

int zri_registry_upsert(zri_registry_t *registry, const char *service, const char *instance_id,
                         const zrpc_endpoint_t *endpoints, size_t endpoint_count,
                         uint32_t node_id, uint64_t last_seen_ms);
void zri_registry_remove_node(zri_registry_t *registry, uint32_t node_id);
int zri_registry_replace_node(zri_registry_t *registry, uint32_t node_id, const char *instance_id,
                               const char *const *services, size_t service_count,
                               const zrpc_endpoint_t *endpoints, size_t endpoint_count,
                               uint64_t last_seen_ms);
void zri_registry_expire(zri_registry_t *registry, uint64_t now_ms, uint64_t ttl_ms);

/* Selector operation: returns one endpoint using registry-local round robin. */
int zri_registry_select(zri_registry_t *registry, const char *service, zrpc_endpoint_t *out);
/* Prefer a TCP endpoint when prefer_stream is set. */
int zri_registry_select_prefer(zri_registry_t *registry, const char *service, int prefer_stream,
                                zrpc_endpoint_t *out);

#endif /* ZRPC_REGISTRY_H */
