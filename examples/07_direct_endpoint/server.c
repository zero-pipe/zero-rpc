/*
 * 07 Direct Endpoint：服务端。对应 fastcall-c 的 07_direct_endpoint。
 * TCP 127.0.0.1:42430，注册 OrderService.get。
 */
#include <server.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <string.h>

static int order_get(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)user;
    (void)req;
    return nb_message_view_set_string(resp, "direct endpoint: ok");
}

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "DirectServer";
    cfg.transport.transport = NB_TRANSPORT_TCP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.transport.data_port = 42430;
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "OrderService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "get", order_get, NULL);
    }
    if (rc != NB_OK) {
        nb_log_error("server failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }
    nb_log_info("direct endpoint server ready on tcp://127.0.0.1:42430 (Ctrl+C to stop)");
    (void)nb_node_spin(node);
    nb_node_destroy(node);
    return 0;
}
