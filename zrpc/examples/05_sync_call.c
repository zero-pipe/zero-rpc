/*
 * 05 Sync Call：同步调用便捷层。对应 fastcall-c 的 fc_proxy_call_sync。
 *
 * 先启动 02_service（默认端口 42410），再运行本示例。
 * 注意：zrpc 核心仍是异步的；zrpc_proxy_call_sync 只是阻塞等待，禁止在
 * poller 线程回调里使用。
 */
#include <zrpc/zrpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    zrpc_node_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_proxy_t *proxy = NULL;
    zrpc_response_t response;
    zrpc_payload_t request;
    char endpoint[64];
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 42410);
    const char *text = "hello";
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "SyncClient";
    cfg.transport = ZRPC_TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;
    rc = zrpc_node_create(&cfg, &node);
    if (rc == ZRPC_OK) {
        rc = zrpc_proxy_create(node, NULL, &proxy);
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "client setup failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }

    snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%u", (unsigned)port);
    if (zrpc_proxy_set_endpoint_uri(proxy, endpoint) != ZRPC_OK) {
        fprintf(stderr, "bad endpoint: %s\n", endpoint);
        zrpc_proxy_destroy(proxy);
        zrpc_node_destroy(node);
        return 1;
    }

    request.data = text;
    request.len = strlen(text);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = zrpc_proxy_call_sync(proxy, "HealthService", "check", &request, &response, 3000, NULL);
    if (rc == ZRPC_OK) {
        printf("response: %.*s\n", (int)response.payload.len,
               (const char *)response.payload.data);
    } else {
        fprintf(stderr, "call failed: %d (server running?)\n", rc);
    }
    zrpc_response_release(&response);
    zrpc_proxy_destroy(proxy);
    zrpc_node_destroy(node);
    return rc == ZRPC_OK ? 0 : 1;
}
