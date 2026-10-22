#ifndef REGISTRY_H
#define REGISTRY_H

#include <endpoint.h>

#include <stddef.h>
#include <stdint.h>

typedef struct nb_registry_instance nb_registry_instance_t;

/*
 * 服务实例注册表：自身即全部状态（含锁），不依赖 node。
 * node 在组合根处内嵌一个 nb_registry_t 并把它传进来。
 */
typedef struct nb_registry {
    nb_registry_instance_t *instances;
    void *lock;
    uint64_t generation;
    size_t next_instance;
} nb_registry_t;

void nb_registry_init(nb_registry_t *registry);
void nb_registry_cleanup(nb_registry_t *registry);

int nb_registry_upsert(nb_registry_t *registry,
                        const char *service,
                        const char *instance_id,
                        const nb_endpoint_t *endpoints,
                        size_t endpoint_count,
                        uint32_t node_id,
                        uint64_t last_seen_ms);
void nb_registry_remove_node(nb_registry_t *registry, uint32_t node_id);
int nb_registry_replace_node(nb_registry_t *registry,
                              uint32_t node_id,
                              const char *instance_id,
                              const char *const *services,
                              size_t service_count,
                              const nb_endpoint_t *endpoints,
                              size_t endpoint_count,
                              uint64_t last_seen_ms);
void nb_registry_expire(nb_registry_t *registry, uint64_t now_ms, uint64_t ttl_ms);

/* Selector operation: returns one endpoint using registry-local round robin. */
int nb_registry_select(nb_registry_t *registry, const char *service, nb_endpoint_t *out);
/* Prefer a TCP endpoint when prefer_stream is set. */
int nb_registry_select_prefer(nb_registry_t *registry,
                               const char *service,
                               int prefer_stream,
                               nb_endpoint_t *out);

#endif /* REGISTRY_H */
