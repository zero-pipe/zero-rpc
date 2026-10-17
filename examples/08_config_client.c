/*
 * 08 Config：客户端。同一套扁平配置，通过 routes 静态指向服务端并同步调用。
 *
 * 用法：example_08_config_client [config.json]
 */
#include <client.h>
#include <config.h>

#define RPC_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "08_config_client.json";
    config_t cfg;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    response_t response;
    payload_t request;
    int rc;

    rc = config_load_file(path, &cfg);
    if (rc != OK) {
        rpc_log_error("load config failed: %s (%d)", path, rc);
        return 1;
    }
    rc = node_create_from_config(&cfg, &node);
    if (rc == OK) {
        rc = proxy_create(node, NULL, &proxy);
    }
    if (rc != OK) {
        rpc_log_error("client setup failed: %d", rc);
        node_destroy(node);
        return 1;
    }

    request.data = "hello config";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = proxy_call_sync(proxy, "EchoService", "echo", &request, &response, 3000, NULL);
    if (rc == OK) {
        rpc_log_info("response: %.*s", (int)response.payload.len,
                     (const char *)response.payload.data);
    } else {
        rpc_log_error("call failed: %d (server running?)", rc);
    }
    response_release(&response);
    proxy_destroy(proxy);
    node_destroy(node);
    return rc == OK ? 0 : 1;
}
