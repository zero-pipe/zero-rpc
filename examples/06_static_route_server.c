/*
 * 06 Static Route：服务端。对应 fastcall-c 的 06_static_route。
 * TCP 127.0.0.1:42420，注册 OrderService.get（同步风格）。
 */
#include <server.h>

#define RPC_LOG_TAG "example"
#include <log.h>

#include <string.h>

static int order_get(const request_t *req, message_view_t *resp, void *user)
{
    (void)user;
    (void)req;
    return message_view_set_string(resp, "static route: ok");
}

int main(void)
{
    node_config_t cfg;
    node_t *node = NULL;
    service_t *svc = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "StaticServer";
    cfg.transport = TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.data_port = 42420;
    cfg.mode = MODE_STATIC;
    cfg.io_threads = 1;

    rc = node_create(&cfg, &node);
    if (rc == OK) {
        rc = service_create(node, "OrderService", &svc);
    }
    if (rc == OK) {
        rc = service_add_sync_method(svc, "get", order_get, NULL);
    }
    if (rc != OK) {
        rpc_log_error("server failed: %d", rc);
        node_destroy(node);
        return 1;
    }
    rpc_log_info("static route server ready on tcp://127.0.0.1:42420 (Ctrl+C to stop)");
    (void)node_spin(node);
    node_destroy(node);
    return 0;
}
