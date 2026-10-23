/*
 * 08 Config：客户端。同一套嵌套配置，通过 routes 静态指向服务端并同步调用。
 *
 * 用法：example_08_config_client [config.json]
 */
#include <client.h>
#include <config.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "client.json";
    nb_config_t cfg;
    nb_config_error_t err;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t request;
    int rc;

    rc = nb_config_load_file_ex(path, &cfg, &err);
    if (rc != NB_OK) {
        nb_log_error(
            "load config failed: %s (%d) path=%s reason=%s", path, rc, err.path, err.reason);
        return 1;
    }
    rc = nb_node_create_from_config(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc != NB_OK) {
        nb_log_error("client setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }

    request.data = "hello config";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = nb_proxy_call_sync(proxy, "EchoService", "echo", &request, &response, 3000, NULL);
    if (rc == NB_OK) {
        nb_log_info(
            "response: %.*s", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        nb_log_error("call failed: %d (server running?)", rc);
    }
    nb_response_release(&response);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
