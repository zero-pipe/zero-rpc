/*
 * 06 Static Route：客户端。对应 fastcall-c 的 06_static_route。
 * 通过 zrpc_node_add_static_route 登记 OrderService -> 127.0.0.1:42420，
 * 跳过服务发现直接路由。
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
    cfg.name = "StaticClient";
    cfg.transport = ZRPC_TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;

    rc = zrpc_node_create(&cfg, &node);
    if (rc == ZRPC_OK) {
        rc = zrpc_node_add_static_route(node, "OrderService", "127.0.0.1", 42420,
                                        ZRPC_TRANSPORT_TCP);
    }
    if (rc == ZRPC_OK) {
        rc = zrpc_proxy_create(node, NULL, &proxy);
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "client setup failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }

    request.data = "order-1001";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = zrpc_proxy_call_sync(proxy, "OrderService", "get", &request, &response, 3000, NULL);
    if (rc == ZRPC_OK) {
        printf("%.*s\n", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        fprintf(stderr, "static route call failed: %d\n", rc);
    }
    zrpc_response_release(&response);
    zrpc_proxy_destroy(proxy);
    zrpc_node_destroy(node);
    return rc == ZRPC_OK ? 0 : 1;
}
