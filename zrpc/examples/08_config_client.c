/*
 * 08 Config：客户端。同一套扁平配置，通过 routes 静态指向服务端并同步调用。
 *
 * 用法：zrpc_example_08_config_client [config.json]
 */
#include <zrpc/zrpc.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "08_config_client.json";
    zrpc_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_proxy_t *proxy = NULL;
    zrpc_response_t response;
    zrpc_payload_t request;
    int rc;

    rc = zrpc_config_load_file(path, &cfg);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "load config failed: %s (%d)\n", path, rc);
        return 1;
    }
    rc = zrpc_node_create_from_config(&cfg, &node);
    if (rc == ZRPC_OK) {
        rc = zrpc_proxy_create(node, NULL, &proxy);
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "client setup failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }

    request.data = "hello config";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = zrpc_proxy_call_sync(proxy, "EchoService", "echo", &request, &response, 3000, NULL);
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
