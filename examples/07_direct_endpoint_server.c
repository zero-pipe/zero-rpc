/*
 * 07 Direct Endpoint：服务端。对应 fastcall-c 的 07_direct_endpoint。
 * TCP 127.0.0.1:42430，注册 OrderService.get。
 */
#include <server.h>

#define RPC_LOG_TAG "example"
#include <log.h>

#include <string.h>

static int order_get(const request_t *req, message_view_t *resp, void *user)
{
    (void)user;
    (void)req;
    return message_view_set_string(resp, "direct endpoint: ok");
}

int main(void)
{
    node_config_t cfg;
    node_t *node = NULL;
    service_t *svc = NULL;
    int rc;

    node_config_default(&cfg);
    cfg.name = "DirectServer";
    cfg.transport.transport = TRANSPORT_TCP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.transport.data_port = 42430;
    cfg.mode = MODE_STATIC;
    cfg.runtime.io_threads = 1;

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
    rpc_log_info("direct endpoint server ready on tcp://127.0.0.1:42430 (Ctrl+C to stop)");
    (void)node_spin(node);
    node_destroy(node);
    return 0;
}
