/*
 * 08 Config：从 JSON 配置创建节点。展示按模块拆分的嵌套配置格式。
 * 服务端：读取 08_config_server.json，注册 EchoService.echo（异步回显）。
 *
 * 用法：example_08_config_server [config.json]
 */
#include <server.h>
#include <config.h>

#define NB_LOG_TAG "example"
#include <log.h>

static void echo_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    /* payload 对框架透明：原样回显。 */
    nb_reply(call, 0, &req->payload);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "server.json";
    nb_config_t cfg;
    nb_config_error_t err;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    int rc;

    rc = nb_config_load_file_ex(path, &cfg, &err);
    if (rc != NB_OK) {
        nb_log_error(
            "load config failed: %s (%d) path=%s reason=%s", path, rc, err.path, err.reason);
        return 1;
    }
    rc = nb_node_create_from_config(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "EchoService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(svc, "echo", echo_handler, NULL);
    }
    if (rc != NB_OK) {
        nb_log_error("server setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }

    nb_log_info("EchoService.echo ready (config=%s, Ctrl+C to stop)", path);
    (void)nb_node_spin(node);
    nb_node_destroy(node);
    return 0;
}
