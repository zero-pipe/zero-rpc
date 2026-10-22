/*
 * 08 Config：从 JSON 配置创建节点。展示按模块拆分的嵌套配置格式。
 * 服务端：读取 08_config_server.json，注册 EchoService.echo（异步回显）。
 *
 * 用法：example_08_config_server [config.json]
 */
#include <server.h>
#include <config.h>

#define RPC_LOG_TAG "example"
#include <log.h>

static void echo_handler(call_t *call, const request_t *req, void *user)
{
    (void)user;
    /* payload 对框架透明：原样回显。 */
    reply(call, 0, &req->payload);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "08_config_server.json";
    config_t cfg;
    node_t *node = NULL;
    service_t *svc = NULL;
    int rc;

    rc = config_load_file(path, &cfg);
    if (rc != OK) {
        rpc_log_error("load config failed: %s (%d)", path, rc);
        return 1;
    }
    rc = node_create_from_config(&cfg, &node);
    if (rc == OK) {
        rc = service_create(node, "EchoService", &svc);
    }
    if (rc == OK) {
        rc = service_add_method(svc, "echo", echo_handler, NULL);
    }
    if (rc != OK) {
        rpc_log_error("server setup failed: %d", rc);
        node_destroy(node);
        return 1;
    }

    rpc_log_info("EchoService.echo ready (config=%s, Ctrl+C to stop)", path);
    (void)node_spin(node);
    node_destroy(node);
    return 0;
}
