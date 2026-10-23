/*
 * 15 SHM：客户端。
 *
 * 通过 shm://zrpc-example:1 直接调用服务端，不启动服务发现。
 */
#include <client.h>
#include <shm.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(void)
{
    nb_binding_config_t binding;
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t request;
    int rc;

    rc = nb_shm_register();
    if (rc != NB_OK) {
        nb_log_error("shm provider register failed: %d (%s)", rc, nb_error_name(rc));
        return 1;
    }

    memset(&binding, 0, sizeof(binding));
    binding.name = "shm";
    binding.endpoint = "shm://zrpc-example:2";
    binding.intra_process = NB_INTRA_OFF;

    nb_node_config_default(&cfg);
    cfg.name = "ShmClient";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_endpoint_uri(proxy, "shm://zrpc-example:1");
    }
    if (rc != NB_OK) {
        nb_log_error("shm client setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    request.data = "hello over shm";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = nb_proxy_call_sync(proxy, "OrderService", "get", &request, &response, 3000, NULL);
    if (rc == NB_OK) {
        nb_log_info("shm response: %.*s",
                    (int)response.payload.len,
                    (const char *)response.payload.data);
    } else {
        nb_log_error("shm call failed: %d (%s)", rc, nb_error_name(rc));
    }

    nb_response_release(&response);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
