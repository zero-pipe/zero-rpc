/*
 * 示例：服务端。注册 echo.ping，原样回显（保留请求的 payload 与 encoding）。
 *
 * 用法：
 *   zrpc_echo_server [config.json]
 * 不带参数时使用内置默认（UDP + 本机发现端口 42300）。
 */
#include <zrpc/zrpc.h>

#include <stdio.h>
#include <string.h>

static void on_echo(zrpc_call_t *call, const zrpc_request_t *req, void *user) {
    (void)user;
    /* payload 对框架透明：原样回显，encoding 一并带回。 */
    zrpc_reply(call, 0, &req->payload);
}

int main(int argc, char **argv) {
    zrpc_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_service_t *svc = NULL;
    int rc;

    if (argc > 1) {
        rc = zrpc_config_load_file(argv[1], &cfg);
        if (rc != ZRPC_OK) {
            fprintf(stderr, "load config failed: %d\n", rc);
            return 1;
        }
    } else {
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.name, sizeof(cfg.name), "echo_server");
        snprintf(cfg.bind_host, sizeof(cfg.bind_host), "127.0.0.1");
        snprintf(cfg.discovery_host, sizeof(cfg.discovery_host), "127.0.0.1");
        cfg.node.name = cfg.name;
        cfg.node.bind_host = cfg.bind_host;
        cfg.node.discovery_host = cfg.discovery_host;
        cfg.node.transport = ZRPC_TRANSPORT_UDP;
        cfg.node.io_threads = 1;
        cfg.node.discovery_port = 42300;
        cfg.node.hello_interval_ms = 200;
    }

    rc = zrpc_node_create_from_config(&cfg, &node);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "node create failed: %d\n", rc);
        return 1;
    }
    rc = zrpc_service_create(node, "echo", &svc);
    if (rc == ZRPC_OK) {
        rc = zrpc_service_add_method(svc, "ping", on_echo, NULL);
    }
    if (rc != ZRPC_OK) {
        fprintf(stderr, "service setup failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }

    printf("ready: service=echo method=ping transport=%s data_port=%u discovery_port=%u\n",
           zrpc_node_transport(node) == ZRPC_TRANSPORT_TCP ? "tcp" : "udp",
           (unsigned)zrpc_node_data_port(node), (unsigned)zrpc_node_discovery_port(node));
    fflush(stdout);

    zrpc_node_spin(node);
    zrpc_node_destroy(node);
    return 0;
}
