/*
 * 07 Direct Endpoint：客户端。对应 fastcall-c 的 07_direct_endpoint。
 * 通过 nb_proxy_set_endpoint_uri 直接指定目标端点，跳过发现与路由表。
 */
#include <client.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t request;
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "DirectClient";
    cfg.transport.transport = NB_TRANSPORT_TCP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_endpoint_uri(proxy, "tcp://127.0.0.1:42430");
    }
    if (rc != NB_OK) {
        nb_log_error("client setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }

    request.data = "direct";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = nb_proxy_call_sync(proxy, "OrderService", "get", &request, &response, 3000, NULL);
    if (rc == NB_OK) {
        nb_log_info("%.*s", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        nb_log_error("direct endpoint call failed: %d", rc);
    }
    nb_response_release(&response);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
