/*
 * 07 Direct Endpoint：服务端。对应 fastcall-c 的 07_direct_endpoint。
 * TCP 127.0.0.1:42430，注册 OrderService.get。
 */
#include <zrpc/zrpc_server.h>

#include <stdio.h>
#include <string.h>

static int order_get(const zrpc_request_t *req, zrpc_message_view_t *resp, void *user) {
    (void)user;
    (void)req;
    return zrpc_message_view_set_string(resp, "direct endpoint: ok");
}

int main(void) {
    zrpc_node_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_service_t *svc = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "DirectServer";
    cfg.transport = ZRPC_TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.data_port = 42430;
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;

    rc = zrpc_node_create(&cfg, &node);
    if (rc == ZRPC_OK) {
        rc = zrpc_service_create(node, "OrderService", &svc);
    }
    if (rc == ZRPC_OK) {
        rc = zrpc_service_add_sync_method(svc, "get", order_get, NULL);
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "server failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }
    puts("direct endpoint server ready on tcp://127.0.0.1:42430 (Ctrl+C to stop)");
    fflush(stdout);
    (void)zrpc_node_spin(node);
    zrpc_node_destroy(node);
    return 0;
}
