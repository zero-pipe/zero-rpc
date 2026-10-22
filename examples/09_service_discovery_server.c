/*
 * 09 Service Discovery：服务端。服务端不要求客户端配置数据端口，
 * 客户端通过 mesh HELLO 发现 DiscoveryService -> TCP endpoint。
 *
 * 用法：example_09_service_discovery_server
 *       [data_port] [discovery_port] [client_ip] [client_discovery_port]
 * 默认 TCP 0.0.0.0:42450，发现端口 41953；本机客户端发现端口 41954。
 */
#include <server.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdlib.h>
#include <string.h>

static int discovery_echo(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)user;
    return nb_message_view_set_data(resp, req->payload.data, req->payload.len);
}

int main(int argc, char **argv)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    uint16_t data_port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 42450);
    uint16_t discovery_port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 41953);
    const char *client_ip = argc > 3 ? argv[3] : "127.0.0.1";
    uint16_t client_discovery_port = (uint16_t)(argc > 4 ? atoi(argv[4]) : 41954);
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "DiscoveryServer";
    cfg.transport.transport = NB_TRANSPORT_TCP;
    cfg.transport.bind_host = "0.0.0.0";
    cfg.transport.data_port = data_port;
    cfg.discovery.host = "255.255.255.255";
    cfg.discovery.port = discovery_port;
    cfg.discovery.hello_interval_ms = 500;
    cfg.discovery.node_ttl_ms = 3000;
    cfg.mode = NB_MODE_MESH;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "DiscoveryService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "echo", discovery_echo, NULL);
    }
    if (rc == NB_OK) {
        rc = nb_node_add_discovery_peer(node, client_ip, client_discovery_port);
    }
    if (rc != NB_OK) {
        nb_log_error("server setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }

    nb_log_info("service=DiscoveryService method=echo data=tcp://0.0.0.0:%u "
                 "discovery=udp://0.0.0.0:%u peer=%s:%u (Ctrl+C to stop)",
                 (unsigned)data_port,
                 (unsigned)discovery_port,
                 client_ip,
                 (unsigned)client_discovery_port);
    (void)nb_node_spin(node);
    nb_node_destroy(node);
    return 0;
}
