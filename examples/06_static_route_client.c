/*
 * 06 Static Route：客户端。对应 fastcall-c 的 06_static_route。
 * 通过 node_add_static_route 登记 OrderService -> 127.0.0.1:42420，
 * 跳过服务发现直接路由。
 */
#include <client.h>

#define RPC_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(void)
{
    node_config_t cfg;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    response_t response;
    payload_t request;
    int rc;

    node_config_default(&cfg);
    cfg.name = "StaticClient";
    cfg.transport.transport = TRANSPORT_TCP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.mode = MODE_STATIC;
    cfg.runtime.io_threads = 1;

    rc = node_create(&cfg, &node);
    if (rc == OK) {
        rc = node_add_static_route(node, "OrderService", "127.0.0.1", 42420, TRANSPORT_TCP);
    }
    if (rc == OK) {
        rc = proxy_create(node, NULL, &proxy);
    }
    if (rc != OK) {
        rpc_log_error("client setup failed: %d", rc);
        node_destroy(node);
        return 1;
    }

    request.data = "order-1001";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = proxy_call_sync(proxy, "OrderService", "get", &request, &response, 3000, NULL);
    if (rc == OK) {
        rpc_log_info("%.*s", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        rpc_log_error("static route call failed: %d", rc);
    }
    response_release(&response);
    proxy_destroy(proxy);
    node_destroy(node);
    return rc == OK ? 0 : 1;
}
