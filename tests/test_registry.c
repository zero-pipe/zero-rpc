/* registry / selector：service instance 隔离、多 endpoint 轮询、删除与过期。 */
#include <runtime.h>

#include "registry.h"

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static nb_endpoint_t ep(const char *scheme, const char *host, uint16_t port)
{
    nb_endpoint_t endpoint;
    memset(&endpoint, 0, sizeof(endpoint));
    snprintf(endpoint.scheme, sizeof(endpoint.scheme), "%s", scheme);
    snprintf(endpoint.host, sizeof(endpoint.host), "%s", host);
    endpoint.port = port;
    endpoint.transport = nb_transport_kind_from_scheme(scheme);
    return endpoint;
}

int main(void)
{
    nb_registry_t registry;
    nb_endpoint_t endpoints[2];
    nb_endpoint_t selected;
    const char *services[2];

    /* 注册表可独立于 node 使用。 */
    memset(&registry, 0, sizeof(registry));
    nb_registry_init(&registry);

    endpoints[0] = ep("udp", "127.0.0.1", 9001);
    endpoints[1] = ep("tcp", "127.0.0.1", 9002);
    services[0] = "order";
    services[1] = "user";
    CHECK(nb_registry_replace_node(
              &registry, 7, "nodeA-00000007", services, 2, endpoints, 2, 1000) == NB_OK);

    /* 一个 instance 有两个 endpoint：对同一 service 轮询。 */
    CHECK(nb_registry_select(&registry, "order", &selected) == NB_OK);
    CHECK(selected.port == 9001);
    CHECK(nb_registry_select(&registry, "order", &selected) == NB_OK);
    CHECK(selected.port == 9002);
    CHECK(strcmp(selected.scheme, "tcp") == 0);

    /* 另一个 service 独立选择。 */
    CHECK(nb_registry_select(&registry, "user", &selected) == NB_OK);
    CHECK(selected.port == 9001);

    /* 大包策略：优先 TCP endpoint。 */
    CHECK(nb_registry_select_prefer(&registry, "order", 1, &selected) == NB_OK);
    CHECK(strcmp(selected.scheme, "tcp") == 0);
    CHECK(selected.port == 9002);

    /* 未知 service。 */
    CHECK(nb_registry_select(&registry, "missing", &selected) == NB_ERR_NOTFOUND);

    /* 整个 node 下线：实例全部移除。 */
    nb_registry_remove_node(&registry, 7);
    CHECK(nb_registry_select(&registry, "order", &selected) == NB_ERR_NOTFOUND);

    /* 过期：last_seen 很早，ttl 之后被清理。 */
    CHECK(nb_registry_replace_node(
              &registry, 9, "nodeB-00000009", services, 1, endpoints, 1, 1000) == NB_OK);
    nb_registry_expire(&registry, 1000 + 5000, 3000);
    CHECK(nb_registry_select(&registry, "order", &selected) == NB_ERR_NOTFOUND);

    nb_registry_cleanup(&registry);

    if (g_fail) {
        nb_log_error("nb_registry: FAIL");
        return 1;
    }
    nb_log_info("nb_registry: NB_OK");
    return 0;
}
