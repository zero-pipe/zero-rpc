/*
 * 02 Service：服务端。注册一个同步风格 handler（返回状态码 + 写 resp view），
 * 框架负责异步回复。对应 fastcall-c 的 02_service / 06_static_route_server。
 *
 * 用法：example_02_service [port]
 * 默认 TCP 127.0.0.1:42410（STATIC 模式，无需发现）。
 */
#include <server.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdlib.h>
#include <string.h>

static int health_check(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)user;
    /* req->payload 对框架透明：这里原样回显。 */
    (void)nb_message_view_set_data(resp, req->payload.data, req->payload.len);
    return 0;
}

int main(int argc, char **argv)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 42410);
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "HealthServer";
    cfg.transport.transport = NB_TRANSPORT_TCP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.transport.data_port = port;
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "HealthService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "check", health_check, NULL);
    }
    if (rc != NB_OK) {
        nb_log_error("server setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }

    nb_log_info("service=HealthService method=check endpoint=tcp://127.0.0.1:%u (Ctrl+C to stop)",
                 (unsigned)port);
    (void)nb_node_spin(node);
    nb_node_destroy(node);
    return 0;
}
