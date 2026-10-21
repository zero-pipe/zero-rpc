/*
 * 09 Service Discovery：客户端。客户端只知道服务端 IP 和发现端口，
 * 不配置服务端数据端口；服务端 endpoint 来自 mesh 服务发现。
 *
 * 用法：example_09_service_discovery_client
 *       [server_ip] [client_discovery_port] [server_discovery_port]
 * 默认连接 127.0.0.1，客户端发现端口 41954，服务端发现端口 41953。
 */
#include <client.h>

#define RPC_LOG_TAG "example"
#include <log.h>

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    node_config_t cfg;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    response_t response;
    payload_t request;
    const char *server_ip = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t client_discovery_port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 41954);
    uint16_t server_discovery_port = (uint16_t)(argc > 3 ? atoi(argv[3]) : 41953);
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "DiscoveryClient";
    cfg.transport = TRANSPORT_TCP;
    cfg.bind_host = "0.0.0.0";
    cfg.data_port = 0;
    cfg.discovery_host = "255.255.255.255";
    cfg.discovery_port = client_discovery_port;
    cfg.hello_interval_ms = 500;
    cfg.node_ttl_ms = 3000;
    cfg.mode = MODE_MESH;
    cfg.io_threads = 1;

    rc = node_create(&cfg, &node);
    if (rc == OK) {
        rc = node_add_discovery_peer(node, server_ip, server_discovery_port);
    }
    if (rc == OK) {
        rc = node_wait_for_service(node, "DiscoveryService", 10000);
    }
    if (rc == OK) {
        rc = proxy_create(node, NULL, &proxy);
    }
    if (rc != OK) {
        rpc_log_error("client discovery/setup failed: %d", rc);
        node_destroy(node);
        return 1;
    }

    request.data = "hello through discovery";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    rc = proxy_call_sync(proxy, "DiscoveryService", "echo", &request, &response, 3000, NULL);
    if (rc == OK) {
        rpc_log_info("discovered call response: %.*s",
                     (int)response.payload.len,
                     (const char *)response.payload.data);
    } else {
        rpc_log_error("discovered call failed: %d", rc);
    }

    response_release(&response);
    proxy_destroy(proxy);
    node_destroy(node);
    return rc == OK ? 0 : 1;
}
