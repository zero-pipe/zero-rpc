/*
 * 09 Service Discovery：客户端。客户端只知道服务端 IP 和发现端口，
 * 不配置服务端数据端口；服务端 endpoint 来自 mesh 服务发现。
 *
 * 用法：example_09_service_discovery_client
 *       [server_ip] [client_discovery_port] [server_discovery_port]
 * 默认连接 127.0.0.1，客户端发现端口 41954，服务端发现端口 41953。
 */
#include <client.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t request;
    const char *server_ip = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t client_discovery_port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 41954);
    uint16_t server_discovery_port = (uint16_t)(argc > 3 ? atoi(argv[3]) : 41953);
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "DiscoveryClient";
    cfg.transport.transport = NB_TRANSPORT_TCP;
    cfg.transport.bind_host = "0.0.0.0";
    cfg.transport.data_port = 0;
    cfg.discovery.host = "255.255.255.255";
    cfg.discovery.port = client_discovery_port;
    cfg.discovery.hello_interval_ms = 500;
    cfg.discovery.node_ttl_ms = 3000;
    cfg.mode = NB_MODE_MESH;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_node_add_discovery_peer(node, server_ip, server_discovery_port);
    }
    if (rc == NB_OK) {
        rc = nb_node_wait_for_service(node, "DiscoveryService", 10000);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc != NB_OK) {
        nb_log_error("client discovery/setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }

    request.data = "hello through discovery";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = nb_proxy_call_sync(proxy, "DiscoveryService", "echo", &request, &response, 3000, NULL);
    if (rc == NB_OK) {
        nb_log_info("discovered call response: %.*s",
                     (int)response.payload.len,
                     (const char *)response.payload.data);
    } else {
        nb_log_error("discovered call failed: %d", rc);
    }

    nb_response_release(&response);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
