/* 服务实例注册表：自带锁，操作对象是 nb_registry_t，不依赖 node。 */
#include "registry.h"

#include "util.h"

#include <ntk.h>

#include <stdlib.h>
#include <string.h>

struct nb_registry_instance {
    nb_registry_instance_t *next;
    char service[NB_SERVICE_NAME_MAX];
    char instance_id[NB_NAME_MAX];
    uint32_t node_id;
    nb_endpoint_t endpoints[NB_MAX_BINDINGS];
    size_t endpoint_count;
    size_t next_endpoint;
    uint64_t last_seen_ms;
};

static void nb_registry_remove_node_locked(nb_registry_t *registry, uint32_t node_id)
{
    nb_registry_instance_t *instance = registry->instances;
    nb_registry_instance_t *previous = NULL;
    nb_registry_instance_t *next;
    while (instance) {
        next = instance->next;
        if (instance->node_id == node_id) {
            if (previous) {
                previous->next = next;
            } else {
                registry->instances = next;
            }
            free(instance);
            registry->generation++;
        } else {
            previous = instance;
        }
        instance = next;
    }
}

static ntk_mutex *nb_registry_lock(const nb_registry_t *registry)
{
    return registry && registry->lock ? (ntk_mutex *)registry->lock : NULL;
}

void nb_registry_init(nb_registry_t *registry)
{
    if (!registry) {
        return;
    }
    registry->instances = NULL;
    registry->generation = 0;
    registry->next_instance = 0;
    registry->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
}

void nb_registry_cleanup(nb_registry_t *registry)
{
    nb_registry_instance_t *instance;
    nb_registry_instance_t *next;
    ntk_mutex *lock;
    if (!registry) {
        return;
    }
    lock = nb_registry_lock(registry);
    if (lock) {
        ntk_mutex_lock(lock);
    }
    instance = registry->instances;
    while (instance) {
        next = instance->next;
        free(instance);
        instance = next;
    }
    registry->instances = NULL;
    registry->next_instance = 0;
    if (lock) {
        ntk_mutex_unlock(lock);
        ntk_mutex_destroy(lock);
    }
    registry->lock = NULL;
}

static nb_registry_instance_t *nb_find_instance(nb_registry_t *registry,
                                              const char *service,
                                              const char *instance_id)
{
    nb_registry_instance_t *instance;
    for (instance = registry->instances; instance; instance = instance->next) {
        if (strcmp(instance->service, service) == 0 &&
            strcmp(instance->instance_id, instance_id) == 0) {
            return instance;
        }
    }
    return NULL;
}

int nb_registry_upsert(nb_registry_t *registry,
                        const char *service,
                        const char *instance_id,
                        const nb_endpoint_t *endpoints,
                        size_t endpoint_count,
                        uint32_t node_id,
                        uint64_t last_seen_ms)
{
    nb_registry_instance_t *instance;
    ntk_mutex *lock;
    if (!registry || !service || !service[0] || !instance_id || !instance_id[0] || !endpoints ||
        endpoint_count == 0 || endpoint_count > NB_MAX_BINDINGS) {
        return NB_ERR_INVALID;
    }
    lock = nb_registry_lock(registry);
    if (lock) {
        ntk_mutex_lock(lock);
    }
    instance = nb_find_instance(registry, service, instance_id);
    if (!instance) {
        instance = (nb_registry_instance_t *)calloc(1, sizeof(*instance));
        if (!instance) {
            if (lock) {
                ntk_mutex_unlock(lock);
            }
            return NB_ERR_NOMEM;
        }
        instance->next = registry->instances;
        registry->instances = instance;
    }
    nb_copy_str(instance->service, sizeof(instance->service), service, "");
    nb_copy_str(instance->instance_id, sizeof(instance->instance_id), instance_id, "");
    instance->node_id = node_id;
    memset(instance->endpoints, 0, sizeof(instance->endpoints));
    memcpy(instance->endpoints, endpoints, endpoint_count * sizeof(endpoints[0]));
    instance->endpoint_count = endpoint_count;
    instance->next_endpoint = 0;
    instance->last_seen_ms = last_seen_ms;
    registry->generation++;
    if (lock) {
        ntk_mutex_unlock(lock);
    }
    return NB_OK;
}

void nb_registry_remove_node(nb_registry_t *registry, uint32_t node_id)
{
    ntk_mutex *lock;
    if (!registry) {
        return;
    }
    lock = nb_registry_lock(registry);
    if (lock) {
        ntk_mutex_lock(lock);
    }
    nb_registry_remove_node_locked(registry, node_id);
    if (lock) {
        ntk_mutex_unlock(lock);
    }
}

void nb_registry_expire(nb_registry_t *registry, uint64_t now_ms, uint64_t ttl_ms)
{
    nb_registry_instance_t *instance;
    nb_registry_instance_t *previous = NULL;
    nb_registry_instance_t *next;
    ntk_mutex *lock;
    if (!registry) {
        return;
    }
    lock = nb_registry_lock(registry);
    if (lock) {
        ntk_mutex_lock(lock);
    }
    instance = registry->instances;
    while (instance) {
        next = instance->next;
        if (now_ms - instance->last_seen_ms > ttl_ms) {
            if (previous) {
                previous->next = next;
            } else {
                registry->instances = next;
            }
            free(instance);
            registry->generation++;
        } else {
            previous = instance;
        }
        instance = next;
    }
    if (lock) {
        ntk_mutex_unlock(lock);
    }
}

static int nb_instance_has_tcp(const nb_registry_instance_t *instance)
{
    size_t i;
    for (i = 0; i < instance->endpoint_count; i++) {
        if (instance->endpoints[i].transport == NB_TRANSPORT_TCP) {
            return 1;
        }
    }
    return 0;
}

int nb_registry_select(nb_registry_t *registry, const char *service, nb_endpoint_t *out)
{
    return nb_registry_select_prefer(registry, service, 0, out);
}

int nb_registry_select_prefer(nb_registry_t *registry,
                               const char *service,
                               int prefer_stream,
                               nb_endpoint_t *out)
{
    nb_registry_instance_t *instance;
    nb_registry_instance_t *selected = NULL;
    size_t count = 0;
    size_t target;
    size_t current = 0;
    ntk_mutex *lock;
    if (!registry || !service || !out) {
        return NB_ERR_INVALID;
    }
    lock = nb_registry_lock(registry);
    if (lock) {
        ntk_mutex_lock(lock);
    }
    for (instance = registry->instances; instance; instance = instance->next) {
        if (strcmp(instance->service, service) == 0 && instance->endpoint_count > 0) {
            if (!prefer_stream || nb_instance_has_tcp(instance)) {
                count++;
            }
        }
    }
    if (count == 0 && prefer_stream) {
        /* Large-payload preference is advisory; fall back to any endpoint. */
        prefer_stream = 0;
        for (instance = registry->instances; instance; instance = instance->next) {
            if (strcmp(instance->service, service) == 0 && instance->endpoint_count > 0) {
                count++;
            }
        }
    }
    if (count == 0) {
        if (lock) {
            ntk_mutex_unlock(lock);
        }
        return NB_ERR_NOTFOUND;
    }
    target = registry->next_instance++ % count;
    for (instance = registry->instances; instance; instance = instance->next) {
        if (strcmp(instance->service, service) == 0 && instance->endpoint_count > 0) {
            if (prefer_stream && !nb_instance_has_tcp(instance)) {
                continue;
            }
            if (current++ == target) {
                selected = instance;
                break;
            }
        }
    }
    if (prefer_stream) {
        size_t i;
        for (i = 0; i < selected->endpoint_count; i++) {
            size_t index = (selected->next_endpoint + i) % selected->endpoint_count;
            if (selected->endpoints[index].transport == NB_TRANSPORT_TCP) {
                *out = selected->endpoints[index];
                selected->next_endpoint = index + 1;
                if (lock) {
                    ntk_mutex_unlock(lock);
                }
                return NB_OK;
            }
        }
    }
    *out = selected->endpoints[selected->next_endpoint++ % selected->endpoint_count];
    if (lock) {
        ntk_mutex_unlock(lock);
    }
    return NB_OK;
}

int nb_registry_replace_node(nb_registry_t *registry,
                              uint32_t node_id,
                              const char *instance_id,
                              const char *const *services,
                              size_t service_count,
                              const nb_endpoint_t *endpoints,
                              size_t endpoint_count,
                              uint64_t last_seen_ms)
{
    size_t i;
    int rc = NB_OK;
    ntk_mutex *lock;
    if (!registry || !instance_id || !instance_id[0] || (service_count && !services) ||
        endpoint_count > NB_MAX_BINDINGS || (service_count && endpoint_count == 0)) {
        return NB_ERR_INVALID;
    }
    lock = nb_registry_lock(registry);
    if (lock) {
        ntk_mutex_lock(lock);
    }
    nb_registry_remove_node_locked(registry, node_id);
    for (i = 0; i < service_count; i++) {
        nb_registry_instance_t *instance;
        if (!services[i] || !services[i][0]) {
            continue;
        }
        instance = (nb_registry_instance_t *)calloc(1, sizeof(*instance));
        if (!instance) {
            rc = NB_ERR_NOMEM;
            break;
        }
        nb_copy_str(instance->service, sizeof(instance->service), services[i], "");
        nb_copy_str(instance->instance_id, sizeof(instance->instance_id), instance_id, "");
        instance->node_id = node_id;
        memcpy(instance->endpoints, endpoints, endpoint_count * sizeof(endpoints[0]));
        instance->endpoint_count = endpoint_count;
        instance->last_seen_ms = last_seen_ms;
        instance->next = registry->instances;
        registry->instances = instance;
        registry->generation++;
    }
    if (lock) {
        ntk_mutex_unlock(lock);
    }
    return rc;
}
