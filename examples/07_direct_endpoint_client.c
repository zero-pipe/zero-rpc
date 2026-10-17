/*
 * 07 Direct Endpoint：客户端。对应 fastcall-c 的 07_direct_endpoint。
 * 通过 proxy_set_endpoint_uri 直接指定目标端点，跳过发现与路由表。
 */
#include <client.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    node_config_t cfg;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    response_t response;
    payload_t request;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "DirectClient";
    cfg.transport = TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = MODE_STATIC;
    cfg.io_threads = 1;

    rc = node_create(&cfg, &node);
    if (rc == OK) {
        rc = proxy_create(node, NULL, &proxy);
    }
    if (rc == OK) {
        rc = proxy_set_endpoint_uri(proxy, "tcp://127.0.0.1:42430");
    }
    if (rc != OK) {
        fprintf(stderr, "client setup failed: %d\n", rc);
        node_destroy(node);
        return 1;
    }

    request.data = "direct";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = proxy_call_sync(proxy, "OrderService", "get", &request, &response, 3000, NULL);
    if (rc == OK) {
        printf("%.*s\n", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        fprintf(stderr, "direct endpoint call failed: %d\n", rc);
    }
    response_release(&response);
    proxy_destroy(proxy);
    node_destroy(node);
    return rc == OK ? 0 : 1;
}
