/*
 * 07 Direct Endpoint：客户端。对应 fastcall-c 的 07_direct_endpoint。
 * 通过 zrpc_proxy_set_endpoint_uri 直接指定目标端点，跳过发现与路由表。
 */
#include <zrpc/zrpc.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    zrpc_node_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_proxy_t *proxy = NULL;
    zrpc_response_t response;
    zrpc_payload_t request;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "DirectClient";
    cfg.transport = ZRPC_TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;

    rc = zrpc_node_create(&cfg, &node);
    if (rc == ZRPC_OK) {
        rc = zrpc_proxy_create(node, NULL, &proxy);
    }
    if (rc == ZRPC_OK) {
        rc = zrpc_proxy_set_endpoint_uri(proxy, "tcp://127.0.0.1:42430");
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "client setup failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }

    request.data = "direct";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = zrpc_proxy_call_sync(proxy, "OrderService", "get", &request, &response, 3000, NULL);
    if (rc == ZRPC_OK) {
        printf("%.*s\n", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        fprintf(stderr, "direct endpoint call failed: %d\n", rc);
    }
    zrpc_response_release(&response);
    zrpc_proxy_destroy(proxy);
    zrpc_node_destroy(node);
    return rc == ZRPC_OK ? 0 : 1;
}
