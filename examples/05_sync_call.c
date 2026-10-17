/*
 * 05 Sync Call：同步调用便捷层。对应 fastcall-c 的 fc_proxy_call_sync。
 *
 * 先启动 02_service（默认端口 42410），再运行本示例。
 * 注意：rpc 核心仍是异步的；proxy_call_sync 只是阻塞等待，禁止在
 * poller 线程回调里使用。
 */
#include <client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    node_config_t cfg;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    response_t response;
    payload_t request;
    char endpoint[64];
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 42410);
    const char *text = "hello";
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "SyncClient";
    cfg.transport = TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = MODE_STATIC;
    cfg.io_threads = 1;
    rc = node_create(&cfg, &node);
    if (rc == OK) {
        rc = proxy_create(node, NULL, &proxy);
    }
    if (rc != OK) {
        fprintf(stderr, "client setup failed: %d\n", rc);
        node_destroy(node);
        return 1;
    }

    snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%u", (unsigned)port);
    if (proxy_set_endpoint_uri(proxy, endpoint) != OK) {
        fprintf(stderr, "bad endpoint: %s\n", endpoint);
        proxy_destroy(proxy);
        node_destroy(node);
        return 1;
    }

    request.data = text;
    request.len = strlen(text);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = proxy_call_sync(proxy, "HealthService", "check", &request, &response, 3000, NULL);
    if (rc == OK) {
        printf("response: %.*s\n", (int)response.payload.len,
               (const char *)response.payload.data);
    } else {
        fprintf(stderr, "call failed: %d (server running?)\n", rc);
    }
    response_release(&response);
    proxy_destroy(proxy);
    node_destroy(node);
    return rc == OK ? 0 : 1;
}
