/* registry / selector：service instance 隔离、多 endpoint 轮询、删除与过期。 */
#include <zrpc/zrpc.h>

#include "zrpc_registry.h"

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

static zrpc_endpoint_t ep(const char *scheme, const char *host, uint16_t port) {
    zrpc_endpoint_t endpoint;
    memset(&endpoint, 0, sizeof(endpoint));
    snprintf(endpoint.scheme, sizeof(endpoint.scheme), "%s", scheme);
    snprintf(endpoint.host, sizeof(endpoint.host), "%s", host);
    endpoint.port = port;
    endpoint.transport = zrpc_transport_kind_from_scheme(scheme);
    return endpoint;
}

int main(void) {
    zrpc_node_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_endpoint_t endpoints[2];
    zrpc_endpoint_t selected;
    const char *services[2];

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "reg-test";
    cfg.transport = ZRPC_TRANSPORT_UDP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;
    if (zrpc_node_create(&cfg, &node) != ZRPC_OK || !node) {
        printf("node create failed\n");
        return 1;
    }

    endpoints[0] = ep("udp", "127.0.0.1", 9001);
    endpoints[1] = ep("tcp", "127.0.0.1", 9002);
    services[0] = "order";
    services[1] = "user";
    CHECK(zrpc_registry_replace_node(node, 7, "nodeA-00000007", services, 2, endpoints, 2, 1000) ==
          ZRPC_OK);

    /* 一个 instance 有两个 endpoint：对同一 service 轮询。 */
    CHECK(zrpc_registry_select(node, "order", &selected) == ZRPC_OK);
    CHECK(selected.port == 9001);
    CHECK(zrpc_registry_select(node, "order", &selected) == ZRPC_OK);
    CHECK(selected.port == 9002);
    CHECK(strcmp(selected.scheme, "tcp") == 0);

    /* 另一个 service 独立选择。 */
    CHECK(zrpc_registry_select(node, "user", &selected) == ZRPC_OK);
    CHECK(selected.port == 9001);

    /* 大包策略：优先 TCP endpoint。 */
    CHECK(zrpc_registry_select_prefer(node, "order", 1, &selected) == ZRPC_OK);
    CHECK(strcmp(selected.scheme, "tcp") == 0);
    CHECK(selected.port == 9002);

    /* 未知 service。 */
    CHECK(zrpc_registry_select(node, "missing", &selected) == ZRPC_ERR_NOTFOUND);

    /* 整个 node 下线：实例全部移除。 */
    zrpc_registry_remove_node(node, 7);
    CHECK(zrpc_registry_select(node, "order", &selected) == ZRPC_ERR_NOTFOUND);

    /* 过期：last_seen 很早，ttl 之后被清理。 */
    CHECK(zrpc_registry_replace_node(node, 9, "nodeB-00000009", services, 1, endpoints, 1, 1000) ==
          ZRPC_OK);
    zrpc_registry_expire(node, 1000 + 5000, 3000);
    CHECK(zrpc_registry_select(node, "order", &selected) == ZRPC_ERR_NOTFOUND);

    zrpc_node_destroy(node);

    if (g_fail) {
        printf("zrpc_registry: FAIL\n");
        return 1;
    }
    printf("zrpc_registry: OK\n");
    return 0;
}
