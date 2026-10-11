/*
 * 08 Config：从 JSON 配置创建节点。展示扁平配置格式（bindings / routes 用 URI）。
 * 服务端：读取 08_config_server.json，注册 EchoService.echo（异步回显）。
 *
 * 用法：zrpc_example_08_config_server [config.json]
 */
#include <zrpc/zrpc.h>

#include <stdio.h>

static void echo_handler(zrpc_call_t *call, const zrpc_request_t *req, void *user) {
    (void)user;
    /* payload 对框架透明：原样回显。 */
    zrpc_reply(call, 0, &req->payload);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "08_config_server.json";
    zrpc_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_service_t *svc = NULL;
    int rc;

    rc = zrpc_config_load_file(path, &cfg);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "load config failed: %s (%d)\n", path, rc);
        return 1;
    }
    rc = zrpc_node_create_from_config(&cfg, &node);
    if (rc == ZRPC_OK) {
        rc = zrpc_service_create(node, "EchoService", &svc);
    }
    if (rc == ZRPC_OK) {
        rc = zrpc_service_add_method(svc, "echo", echo_handler, NULL);
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "server setup failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }

    printf("EchoService.echo ready (config=%s, Ctrl+C to stop)\n", path);
    fflush(stdout);
    (void)zrpc_node_spin(node);
    zrpc_node_destroy(node);
    return 0;
}
