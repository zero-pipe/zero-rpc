/* registry / selector：service instance 隔离、多 endpoint 轮询、删除与过期。 */
#include <runtime.h>

#include "registry.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                            \
        }                                                          \
    } while (0)

static endpoint_t ep(const char *scheme, const char *host, uint16_t port) {
    endpoint_t endpoint;
    memset(&endpoint, 0, sizeof(endpoint));
    snprintf(endpoint.scheme, sizeof(endpoint.scheme), "%s", scheme);
    snprintf(endpoint.host, sizeof(endpoint.host), "%s", host);
    endpoint.port = port;
    endpoint.transport = transport_kind_from_scheme(scheme);
    return endpoint;
}

int main(void) {
    zri_registry_t registry;
    endpoint_t endpoints[2];
    endpoint_t selected;
    const char *services[2];

    /* 注册表可独立于 node 使用。 */
    memset(&registry, 0, sizeof(registry));
    zri_registry_init(&registry);

    endpoints[0] = ep("udp", "127.0.0.1", 9001);
    endpoints[1] = ep("tcp", "127.0.0.1", 9002);
    services[0] = "order";
    services[1] = "user";
    CHECK(zri_registry_replace_node(&registry, 7, "nodeA-00000007", services, 2, endpoints, 2,
                                     1000) == OK);

    /* 一个 instance 有两个 endpoint：对同一 service 轮询。 */
    CHECK(zri_registry_select(&registry, "order", &selected) == OK);
    CHECK(selected.port == 9001);
    CHECK(zri_registry_select(&registry, "order", &selected) == OK);
    CHECK(selected.port == 9002);
    CHECK(strcmp(selected.scheme, "tcp") == 0);

    /* 另一个 service 独立选择。 */
    CHECK(zri_registry_select(&registry, "user", &selected) == OK);
    CHECK(selected.port == 9001);

    /* 大包策略：优先 TCP endpoint。 */
    CHECK(zri_registry_select_prefer(&registry, "order", 1, &selected) == OK);
    CHECK(strcmp(selected.scheme, "tcp") == 0);
    CHECK(selected.port == 9002);

    /* 未知 service。 */
    CHECK(zri_registry_select(&registry, "missing", &selected) == ERR_NOTFOUND);

    /* 整个 node 下线：实例全部移除。 */
    zri_registry_remove_node(&registry, 7);
    CHECK(zri_registry_select(&registry, "order", &selected) == ERR_NOTFOUND);

    /* 过期：last_seen 很早，ttl 之后被清理。 */
    CHECK(zri_registry_replace_node(&registry, 9, "nodeB-00000009", services, 1, endpoints, 1,
                                     1000) == OK);
    zri_registry_expire(&registry, 1000 + 5000, 3000);
    CHECK(zri_registry_select(&registry, "order", &selected) == ERR_NOTFOUND);

    zri_registry_cleanup(&registry);

    if (g_fail) {
        printf("zri_registry: FAIL\n");
        return 1;
    }
    printf("zri_registry: OK\n");
    return 0;
}
