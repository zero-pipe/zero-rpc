/*
 * 02 Service：服务端。注册一个同步风格 handler（返回状态码 + 写 resp view），
 * 框架负责异步回复。对应 fastcall-c 的 02_service / 06_static_route_server。
 *
 * 用法：zrpc_example_02_service [port]
 * 默认 TCP 127.0.0.1:42410（STATIC 模式，无需发现）。
 */
#include <zrpc/zrpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int health_check(const zrpc_request_t *req, zrpc_message_view_t *resp, void *user) {
    (void)user;
    /* req->payload 对框架透明：这里原样回显。 */
    (void)zrpc_message_view_set_data(resp, req->payload.data, req->payload.len);
    return 0;
}

int main(int argc, char **argv) {
    zrpc_node_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_service_t *svc = NULL;
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 42410);
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "HealthServer";
    cfg.transport = ZRPC_TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.data_port = port;
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;

    rc = zrpc_node_create(&cfg, &node);
    if (rc == ZRPC_OK) {
        rc = zrpc_service_create(node, "HealthService", &svc);
    }
    if (rc == ZRPC_OK) {
        rc = zrpc_service_add_sync_method(svc, "check", health_check, NULL);
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "server setup failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }

    printf("service=HealthService method=check endpoint=tcp://127.0.0.1:%u (Ctrl+C to stop)\n",
           (unsigned)port);
    fflush(stdout);
    (void)zrpc_node_spin(node);
    zrpc_node_destroy(node);
    return 0;
}
