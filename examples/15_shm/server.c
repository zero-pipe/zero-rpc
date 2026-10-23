/*
 * 15 SHM：服务端。
 *
 * 服务端在 shm://zrpc-example:1 上监听，客户端使用 :2。两个进程必须使用
 * 相同的 channel 名，并且先启动服务端。Linux POSIX 共享内存实现。
 */
#include <server.h>
#include <shm.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <string.h>

static int order_get(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)user;
    return nb_message_view_set_data(resp, req->payload.data, req->payload.len);
}

int main(void)
{
    nb_binding_config_t binding;
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    int rc;

    rc = nb_shm_register();
    if (rc != NB_OK) {
        nb_log_error("shm provider register failed: %d (%s)", rc, nb_error_name(rc));
        return 1;
    }

    memset(&binding, 0, sizeof(binding));
    binding.name = "shm";
    binding.endpoint = "shm://zrpc-example:1";
    binding.intra_process = NB_INTRA_OFF;

    nb_node_config_default(&cfg);
    cfg.name = "ShmServer";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "OrderService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "get", order_get, NULL);
    }
    if (rc != NB_OK) {
        nb_log_error("shm server setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    nb_log_info("shm server ready on shm://zrpc-example:1 (Ctrl+C to stop)");
    (void)nb_node_spin(node);
    nb_service_destroy(svc);
    nb_node_destroy(node);
    return 0;
}
