/*
 * zrpc benchmark server.
 *
 * A test fixture mirroring the CLI/readiness contract of
 * tests/perf/framework/rpc_bench_server.c (OrderService.get), so the same
 * load generator conventions apply. Not a shipped example.
 */
#include <zrpc/zrpc.h>
#include <ztk/platform.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop;

typedef struct bench_options {
    size_t response_bytes;
    size_t delay_ms;
    int echo_request;
} bench_options_t;

static void on_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static const char k_pattern[] = "zrpc-bench-payload-pattern";

static void order_get(zrpc_call_t *call, const zrpc_request_t *req, void *user) {
    const bench_options_t *options = (const bench_options_t *)user;
    zrpc_payload_t pl;

    /* Fault-injection hook for timeout tests: stall the handler before replying. */
    if (options && options->delay_ms) {
        ztk_sleep_ms((unsigned)options->delay_ms);
    }
    if (options && options->echo_request) {
        zrpc_reply(call, 0, &req->payload);
        return;
    }
    if (options && options->response_bytes != 0) {
        size_t i;
        size_t length = options->response_bytes;
        char *body = (char *)malloc(length);
        if (!body) {
            zrpc_reply(call, ZRPC_ERR_NOMEM, NULL);
            return;
        }
        for (i = 0; i < length; i++) {
            body[i] = k_pattern[i % (sizeof(k_pattern) - 1)];
        }
        pl.data = body;
        pl.len = length;
        pl.encoding = 0;
        zrpc_reply(call, 0, &pl);
        free(body);
        return;
    }
    zrpc_reply_bytes(call, 0, "order:confirmed", strlen("order:confirmed"));
}

static int parse_size(const char *value, size_t *out) {
    char *end = NULL;
    unsigned long long parsed;
    if (!value || !out || value[0] == '\0') {
        return -1;
    }
    parsed = strtoull(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0) {
        return -1;
    }
    *out = (size_t)parsed;
    return 0;
}

int main(int argc, char **argv) {
    zrpc_node_config_t node_cfg;
    zrpc_node_t *node = NULL;
    zrpc_service_t *service = NULL;
    bench_options_t options;
    const char *service_name = "OrderService";
    const char *method_name = "get";
    const char *config = NULL;
    char peer_hosts[16][64];
    uint16_t peer_ports[16];
    int peer_count = 0;
    char *colon;
    int rc;
    int index;

    memset(&options, 0, sizeof(options));
    memset(&node_cfg, 0, sizeof(node_cfg));
    node_cfg.name = "zrpc_bench_server";
    node_cfg.bind_host = "127.0.0.1";
    node_cfg.discovery_host = "127.0.0.1";
    node_cfg.discovery_port = ZRPC_DISCOVERY_PORT;
    node_cfg.io_threads = 2;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            config = argv[++index];
            (void)config; /* zrpc 不使用 fastcall 配置；保留以兼容脚本调用。 */
        } else if (strcmp(argv[index], "--transport") == 0 && index + 1 < argc) {
            const char *t = argv[++index];
            node_cfg.transport =
                (strcmp(t, "tcp") == 0 || strcmp(t, "TCP") == 0) ? ZRPC_TRANSPORT_TCP
                                                                 : ZRPC_TRANSPORT_UDP;
        } else if (strcmp(argv[index], "--io-threads") == 0 && index + 1 < argc) {
            node_cfg.io_threads = (unsigned)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--response-bytes") == 0 && index + 1 < argc) {
            if (parse_size(argv[++index], &options.response_bytes) != 0) {
                fprintf(stderr, "invalid --response-bytes value\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--delay-ms") == 0 && index + 1 < argc) {
            options.delay_ms = (size_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--echo-request") == 0) {
            options.echo_request = 1;
        } else if (strcmp(argv[index], "--service") == 0 && index + 1 < argc) {
            service_name = argv[++index];
        } else if (strcmp(argv[index], "--method") == 0 && index + 1 < argc) {
            method_name = argv[++index];
        } else if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            node_cfg.bind_host = argv[++index];
        } else if (strcmp(argv[index], "--discovery-port") == 0 && index + 1 < argc) {
            node_cfg.discovery_port = (uint16_t)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--data-port") == 0 && index + 1 < argc) {
            node_cfg.data_port = (uint16_t)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--max-msg-bytes") == 0 && index + 1 < argc) {
            node_cfg.max_msg_bytes = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--backpressure-bytes") == 0 && index + 1 < argc) {
            node_cfg.backpressure_bytes = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--frag-bytes") == 0 && index + 1 < argc) {
            node_cfg.frag_bytes = (uint16_t)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--peer") == 0 && index + 1 < argc) {
            if (peer_count < 16) {
                snprintf(peer_hosts[peer_count], sizeof(peer_hosts[0]), "%s", argv[++index]);
                colon = strrchr(peer_hosts[peer_count], ':');
                if (colon) {
                    *colon = '\0';
                    peer_ports[peer_count] = (uint16_t)atoi(colon + 1);
                    peer_count++;
                }
            } else {
                index++;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--config <file>] [--transport udp|tcp] [--response-bytes <n>] "
                    "[--delay-ms <n>] [--echo-request] "
                    "[--service <name>] [--method <name>] [--bind <ip>] [--io-threads <n>] "
                    "[--discovery-port <p>] [--data-port <p>] [--max-msg-bytes <n>] "
                    "[--backpressure-bytes <n>] [--peer <ip:port>]\n",
                    argv[0]);
            return 1;
        }
    }
    if (signal(SIGINT, on_signal) == SIG_ERR || signal(SIGTERM, on_signal) == SIG_ERR) {
        return 1;
    }

    rc = zrpc_node_create(&node_cfg, &node);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "zrpc_node_create failed: %d\n", rc);
        goto done;
    }
    for (index = 0; index < peer_count; index++) {
        (void)zrpc_node_add_discovery_peer(node, peer_hosts[index], peer_ports[index]);
    }
    rc = zrpc_service_create(node, service_name, &service);
    if (rc == ZRPC_OK) {
        rc = zrpc_service_add_method(service, method_name, order_get, &options);
    }
    if (rc != ZRPC_OK) {
        goto done;
    }

    printf("ready: service=%s method=%s transport=%s data_port=%u discovery_port=%u\n", service_name,
           method_name, zrpc_node_transport(node) == ZRPC_TRANSPORT_TCP ? "tcp" : "udp",
           (unsigned)zrpc_node_data_port(node), (unsigned)zrpc_node_discovery_port(node));
    fflush(stdout);

    while (!g_stop) {
        ztk_sleep_ms(100);
    }

done:
    zrpc_node_destroy(node);
    return rc == ZRPC_OK ? 0 : 1;
}
