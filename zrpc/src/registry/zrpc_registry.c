#include "zrpc_registry.h"

#include "zrpc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct zrpc_registry_instance {
    zrpc_registry_instance_t *next;
    char service[ZRPC_SERVICE_NAME_MAX];
    char instance_id[ZRPC_NAME_MAX];
    uint32_t node_id;
    zrpc_endpoint_t endpoints[ZRPC_MAX_BINDINGS];
    size_t endpoint_count;
    size_t next_endpoint;
    uint64_t last_seen_ms;
};

static void registry_remove_node_locked(zrpc_node_t *node, uint32_t node_id) {
    zrpc_registry_instance_t *instance = node->registry.instances;
    zrpc_registry_instance_t *previous = NULL;
    zrpc_registry_instance_t *next;
    while (instance) {
        next = instance->next;
        if (instance->node_id == node_id) {
            if (previous) {
                previous->next = next;
            } else {
                node->registry.instances = next;
            }
            free(instance);
            node->registry.generation++;
        } else {
            previous = instance;
        }
        instance = next;
    }
}

static ztk_mutex *registry_lock(const zrpc_node_t *node) {
    return node && node->registry.lock ? (ztk_mutex *)node->registry.lock : NULL;
}

void zrpc_registry_init(zrpc_node_t *node) {
    if (!node) {
        return;
    }
    node->registry.instances = NULL;
    node->registry.generation = 0;
    node->registry.next_instance = 0;
    node->registry.lock = ztk_mutex_create(ZTK_MUTEX_NORMAL);
}

void zrpc_registry_cleanup(zrpc_node_t *node) {
    zrpc_registry_instance_t *instance;
    zrpc_registry_instance_t *next;
    ztk_mutex *lock;
    if (!node) {
        return;
    }
    lock = registry_lock(node);
    if (lock) {
        ztk_mutex_lock(lock);
    }
    instance = node->registry.instances;
    while (instance) {
        next = instance->next;
        free(instance);
        instance = next;
    }
    node->registry.instances = NULL;
    node->registry.next_instance = 0;
    if (lock) {
        ztk_mutex_unlock(lock);
        ztk_mutex_destroy(lock);
    }
    node->registry.lock = NULL;
}

static zrpc_registry_instance_t *find_instance(zrpc_node_t *node, const char *service,
                                               const char *instance_id) {
    zrpc_registry_instance_t *instance;
    for (instance = node->registry.instances; instance; instance = instance->next) {
        if (strcmp(instance->service, service) == 0 &&
            strcmp(instance->instance_id, instance_id) == 0) {
            return instance;
        }
    }
    return NULL;
}

int zrpc_registry_upsert(zrpc_node_t *node, const char *service, const char *instance_id,
                         const zrpc_endpoint_t *endpoints, size_t endpoint_count,
                         uint32_t node_id, uint64_t last_seen_ms) {
    zrpc_registry_instance_t *instance;
    ztk_mutex *lock;
    if (!node || !service || !service[0] || !instance_id || !instance_id[0] ||
        !endpoints || endpoint_count == 0 || endpoint_count > ZRPC_MAX_BINDINGS) {
        return ZRPC_ERR_INVALID;
    }
    lock = registry_lock(node);
    if (lock) {
        ztk_mutex_lock(lock);
    }
    instance = find_instance(node, service, instance_id);
    if (!instance) {
        instance = (zrpc_registry_instance_t *)calloc(1, sizeof(*instance));
        if (!instance) {
            if (lock) {
                ztk_mutex_unlock(lock);
            }
            return ZRPC_ERR_NOMEM;
        }
        instance->next = node->registry.instances;
        node->registry.instances = instance;
    }
    zrpc_copy_str(instance->service, sizeof(instance->service), service, "");
    zrpc_copy_str(instance->instance_id, sizeof(instance->instance_id), instance_id, "");
    instance->node_id = node_id;
    memset(instance->endpoints, 0, sizeof(instance->endpoints));
    memcpy(instance->endpoints, endpoints, endpoint_count * sizeof(endpoints[0]));
    instance->endpoint_count = endpoint_count;
    instance->next_endpoint = 0;
    instance->last_seen_ms = last_seen_ms;
    node->registry.generation++;
    if (lock) {
        ztk_mutex_unlock(lock);
    }
    return ZRPC_OK;
}

void zrpc_registry_remove_node(zrpc_node_t *node, uint32_t node_id) {
    ztk_mutex *lock;
    if (!node) {
        return;
    }
    lock = registry_lock(node);
    if (lock) {
        ztk_mutex_lock(lock);
    }
    registry_remove_node_locked(node, node_id);
    if (lock) {
        ztk_mutex_unlock(lock);
    }
}

void zrpc_registry_expire(zrpc_node_t *node, uint64_t now_ms, uint64_t ttl_ms) {
    zrpc_registry_instance_t *instance;
    zrpc_registry_instance_t *previous = NULL;
    zrpc_registry_instance_t *next;
    ztk_mutex *lock;
    if (!node) {
        return;
    }
    lock = registry_lock(node);
    if (lock) {
        ztk_mutex_lock(lock);
    }
    instance = node->registry.instances;
    while (instance) {
        next = instance->next;
        if (now_ms - instance->last_seen_ms > ttl_ms) {
            if (previous) {
                previous->next = next;
            } else {
                node->registry.instances = next;
            }
            free(instance);
            node->registry.generation++;
        } else {
            previous = instance;
        }
        instance = next;
    }
    if (lock) {
        ztk_mutex_unlock(lock);
    }
}

static int instance_has_tcp(const zrpc_registry_instance_t *instance) {
    size_t i;
    for (i = 0; i < instance->endpoint_count; i++) {
        if (instance->endpoints[i].transport == ZRPC_TRANSPORT_TCP) {
            return 1;
        }
    }
    return 0;
}

int zrpc_registry_select(zrpc_node_t *node, const char *service, zrpc_endpoint_t *out) {
    return zrpc_registry_select_prefer(node, service, 0, out);
}

int zrpc_registry_select_prefer(zrpc_node_t *node, const char *service, int prefer_stream,
                                zrpc_endpoint_t *out) {
    zrpc_registry_instance_t *instance;
    zrpc_registry_instance_t *selected = NULL;
    size_t count = 0;
    size_t target;
    size_t current = 0;
    ztk_mutex *lock;
    if (!node || !service || !out) {
        return ZRPC_ERR_INVALID;
    }
    lock = registry_lock(node);
    if (lock) {
        ztk_mutex_lock(lock);
    }
    for (instance = node->registry.instances; instance; instance = instance->next) {
        if (strcmp(instance->service, service) == 0 && instance->endpoint_count > 0) {
            if (!prefer_stream || instance_has_tcp(instance)) {
                count++;
            }
        }
    }
    if (count == 0 && prefer_stream) {
        /* Large-payload preference is advisory; fall back to any endpoint. */
        prefer_stream = 0;
        for (instance = node->registry.instances; instance; instance = instance->next) {
            if (strcmp(instance->service, service) == 0 && instance->endpoint_count > 0) {
                count++;
            }
        }
    }
    if (count == 0) {
        if (lock) {
            ztk_mutex_unlock(lock);
        }
        return ZRPC_ERR_NOTFOUND;
    }
    target = node->registry.next_instance++ % count;
    for (instance = node->registry.instances; instance; instance = instance->next) {
        if (strcmp(instance->service, service) == 0 && instance->endpoint_count > 0) {
            if (prefer_stream && !instance_has_tcp(instance)) {
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
            if (selected->endpoints[index].transport == ZRPC_TRANSPORT_TCP) {
                *out = selected->endpoints[index];
                selected->next_endpoint = index + 1;
                if (lock) {
                    ztk_mutex_unlock(lock);
                }
                return ZRPC_OK;
            }
        }
    }
    *out = selected->endpoints[selected->next_endpoint++ % selected->endpoint_count];
    if (lock) {
        ztk_mutex_unlock(lock);
    }
    return ZRPC_OK;
}

int zrpc_registry_replace_node(zrpc_node_t *node, uint32_t node_id, const char *instance_id,
                               const char *const *services, size_t service_count,
                               const zrpc_endpoint_t *endpoints, size_t endpoint_count,
                               uint64_t last_seen_ms) {
    size_t i;
    int rc = ZRPC_OK;
    ztk_mutex *lock;
    if (!node || !instance_id || !instance_id[0] || (service_count && !services) ||
        endpoint_count > ZRPC_MAX_BINDINGS || (service_count && endpoint_count == 0)) {
        return ZRPC_ERR_INVALID;
    }
    lock = registry_lock(node);
    if (lock) {
        ztk_mutex_lock(lock);
    }
    registry_remove_node_locked(node, node_id);
    for (i = 0; i < service_count; i++) {
        zrpc_registry_instance_t *instance;
        if (!services[i] || !services[i][0]) {
            continue;
        }
        instance = (zrpc_registry_instance_t *)calloc(1, sizeof(*instance));
        if (!instance) {
            rc = ZRPC_ERR_NOMEM;
            break;
        }
        zrpc_copy_str(instance->service, sizeof(instance->service), services[i], "");
        zrpc_copy_str(instance->instance_id, sizeof(instance->instance_id), instance_id, "");
        instance->node_id = node_id;
        memcpy(instance->endpoints, endpoints, endpoint_count * sizeof(endpoints[0]));
        instance->endpoint_count = endpoint_count;
        instance->last_seen_ms = last_seen_ms;
        instance->next = node->registry.instances;
        node->registry.instances = instance;
        node->registry.generation++;
    }
    if (lock) {
        ztk_mutex_unlock(lock);
    }
    return rc;
}
