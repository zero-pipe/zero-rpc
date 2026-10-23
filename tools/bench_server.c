/*
 * rpc benchmark server.
 *
 * A test fixture mirroring the CLI/readiness contract of
 * tests/perf/framework/rpc_bench_server.c (OrderService.get), so the same
 * load generator conventions apply. Not a shipped example.
 */
#include <server.h>
#include <dds.h>
#include <shm.h>
#include <platform.h>
#include <thread/thread_pool.h>

#define NB_LOG_TAG "bench"
#include <log.h>

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

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static const char k_pattern[] = "rpc-bench-payload-pattern";

/* 自定义 executor：用一个独立的 ntk_thread_pool 承载阻塞方法。 */
static int executor_submit(void *ctx, void (*fn)(void *), void *user)
{
    ntk_thread_pool *pool = (ntk_thread_pool *)ctx;
    return ntk_thread_pool_async(pool, fn, user, 0) == NTK_OK ? NB_OK : NB_ERR_IO;
}

static void order_get(nb_call_t *call, const nb_request_t *req, void *user)
{
    const bench_options_t *options = (const bench_options_t *)user;
    nb_payload_t pl;

    /* Fault-injection hook for timeout tests: stall the handler before replying. */
    if (options && options->delay_ms) {
        ntk_sleep_ms((unsigned)options->delay_ms);
    }
    if (options && options->echo_request) {
        nb_reply(call, 0, &req->payload);
        return;
    }
    if (options && options->response_bytes != 0) {
        size_t i;
        size_t length = options->response_bytes;
        char *body = (char *)malloc(length);
        if (!body) {
            nb_reply(call, NB_ERR_NOMEM, NULL);
            return;
        }
        for (i = 0; i < length; i++) {
            body[i] = k_pattern[i % (sizeof(k_pattern) - 1)];
        }
        pl.data = body;
        pl.len = length;
        pl.encoding = 0;
        nb_reply(call, 0, &pl);
        free(body);
        return;
    }
    nb_reply_bytes(call, 0, "order:confirmed", strlen("order:confirmed"));
}

static int parse_size(const char *value, size_t *out)
{
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

int main(int argc, char **argv)
{
    nb_node_config_t node_cfg;
    nb_node_t *node = NULL;
    nb_service_t *service = NULL;
    bench_options_t fast_options;
    bench_options_t block_options;
    const char *service_name = "OrderService";
    const char *method_name = "get";
    const char *block_method_name = NULL;
    char peer_hosts[16][64];
    uint16_t peer_ports[16];
    int peer_count = 0;
    char *colon;
    int rc;
    int index;
    int use_worker = 0;
    int use_dds = 0;
    int use_shm = 0;
    int dds_domain = 0;
    const char *shm_channel = "zrpc-shm-bench";
    char dds_endpoint[32];
    char shm_endpoint[128];
    nb_binding_config_t dds_binding;
    nb_binding_config_t shm_binding;
    unsigned main_executor_threads = 0;
    int block_worker = 0;
    size_t block_delay_ms = 0;
    unsigned block_executor_threads = 0;
    nb_node_mode_t mode = NB_MODE_MESH;
    ntk_thread_pool *main_executor_pool = NULL;
    nb_service_executor_t main_executor;
    ntk_thread_pool *block_executor_pool = NULL;
    nb_service_executor_t block_executor;

    memset(&fast_options, 0, sizeof(fast_options));
    memset(&block_options, 0, sizeof(block_options));
    memset(&main_executor, 0, sizeof(main_executor));
    memset(&block_executor, 0, sizeof(block_executor));
    block_options.echo_request = 1;
    nb_node_config_default(&node_cfg);
    node_cfg.name = "bench_server";
    node_cfg.transport.bind_host = "127.0.0.1";
    node_cfg.discovery.host = "127.0.0.1";
    node_cfg.discovery.port = NB_DISCOVERY_PORT;
    node_cfg.runtime.io_threads = 0; /* 0 = auto (hardware_concurrency); O1: was 2 */

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--transport") == 0 && index + 1 < argc) {
            const char *t = argv[++index];
            node_cfg.transport.transport =
                (strcmp(t, "tcp") == 0 || strcmp(t, "TCP") == 0) ? NB_TRANSPORT_TCP : NB_TRANSPORT_UDP;
        } else if (strcmp(argv[index], "--io-threads") == 0 && index + 1 < argc) {
            node_cfg.runtime.io_threads = (unsigned)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--mode") == 0 && index + 1 < argc) {
            mode = (strcmp(argv[++index], "static") == 0) ? NB_MODE_STATIC : NB_MODE_MESH;
        } else if (strcmp(argv[index], "--handler-threads") == 0 && index + 1 < argc) {
            node_cfg.runtime.handler_threads = (unsigned)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--handler-max-concurrency") == 0 && index + 1 < argc) {
            node_cfg.runtime.handler_max_concurrency = (unsigned)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--worker") == 0) {
            use_worker = 1;
        } else if (strcmp(argv[index], "--dds") == 0) {
            use_dds = 1;
        } else if (strcmp(argv[index], "--shm") == 0) {
            use_shm = 1;
        } else if (strcmp(argv[index], "--shm-channel") == 0 && index + 1 < argc) {
            shm_channel = argv[++index];
        } else if (strcmp(argv[index], "--dds-domain") == 0 && index + 1 < argc) {
            dds_domain = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--executor") == 0 && index + 1 < argc) {
            main_executor_threads = (unsigned)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--block-method") == 0 && index + 1 < argc) {
            block_method_name = argv[++index];
        } else if (strcmp(argv[index], "--block-delay-ms") == 0 && index + 1 < argc) {
            block_delay_ms = (size_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--block-worker") == 0) {
            block_worker = 1;
        } else if (strcmp(argv[index], "--block-executor") == 0 && index + 1 < argc) {
            block_executor_threads = (unsigned)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--response-bytes") == 0 && index + 1 < argc) {
            if (parse_size(argv[++index], &fast_options.response_bytes) != 0) {
                nb_log_error("invalid --response-bytes value");
                return 1;
            }
        } else if (strcmp(argv[index], "--delay-ms") == 0 && index + 1 < argc) {
            fast_options.delay_ms = (size_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--echo-request") == 0) {
            fast_options.echo_request = 1;
        } else if (strcmp(argv[index], "--service") == 0 && index + 1 < argc) {
            service_name = argv[++index];
        } else if (strcmp(argv[index], "--method") == 0 && index + 1 < argc) {
            method_name = argv[++index];
        } else if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            node_cfg.transport.bind_host = argv[++index];
        } else if (strcmp(argv[index], "--discovery-port") == 0 && index + 1 < argc) {
            node_cfg.discovery.port = (uint16_t)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--data-port") == 0 && index + 1 < argc) {
            node_cfg.transport.data_port = (uint16_t)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--max-msg-bytes") == 0 && index + 1 < argc) {
            node_cfg.transport.max_msg_bytes = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--backpressure-bytes") == 0 && index + 1 < argc) {
            node_cfg.transport.backpressure_bytes = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--frag-bytes") == 0 && index + 1 < argc) {
            node_cfg.transport.frag_bytes = (uint16_t)atoi(argv[++index]);
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
            nb_log_error("usage: %s [--transport udp|tcp] [--response-bytes <n>] "
                          "[--delay-ms <n>] [--echo-request] "
                          "[--block-method <name>] [--block-delay-ms <n>] [--block-worker] "
                          "[--block-executor <threads>] "
                          "[--mode static|mesh] "
                          "[--service <name>] [--method <name>] [--bind <ip>] [--io-threads <n>] "
                          "[--handler-threads <n>] [--handler-max-concurrency <n>] [--worker] "
                          "[--dds] [--dds-domain <n>] [--shm] [--shm-channel <name>] "
                          "[--executor <threads>] "
                          "[--discovery-port <p>] [--data-port <p>] [--max-msg-bytes <n>] "
                          "[--backpressure-bytes <n>] [--peer <ip:port>]",
                          argv[0]);
            return 1;
        }
    }
    if (signal(SIGINT, on_signal) == SIG_ERR || signal(SIGTERM, on_signal) == SIG_ERR) {
        return 1;
    }

    if (use_shm && use_dds) {
        nb_log_error("--shm and --dds are mutually exclusive");
        return 1;
    }
    if (use_shm) {
        if (nb_shm_register() != NB_OK) {
            nb_log_error("shm provider register failed");
            return 1;
        }
        snprintf(shm_endpoint, sizeof(shm_endpoint), "shm://%s:1", shm_channel);
        memset(&shm_binding, 0, sizeof(shm_binding));
        shm_binding.name = "shm";
        shm_binding.endpoint = shm_endpoint;
        shm_binding.intra_process = NB_INTRA_OFF;
        node_cfg.transport.bindings = &shm_binding;
        node_cfg.transport.binding_count = 1;
        mode = NB_MODE_STATIC;
    } else if (use_dds) {
        if (nb_cyclone_dds_register() != NB_OK) {
            nb_log_error("dds provider register failed");
            return 1;
        }
        snprintf(dds_endpoint, sizeof(dds_endpoint), "dds://%d:1", dds_domain);
        memset(&dds_binding, 0, sizeof(dds_binding));
        dds_binding.name = "dds";
        dds_binding.endpoint = dds_endpoint;
        dds_binding.intra_process = NB_INTRA_OFF;
        node_cfg.transport.bindings = &dds_binding;
        node_cfg.transport.binding_count = 1;
        mode = NB_MODE_STATIC;
    }
    node_cfg.mode = mode;
    rc = nb_node_create(&node_cfg, &node);
    if (rc != NB_OK) {
        nb_log_error("nb_node_create failed: %d", rc);
        goto done;
    }
    for (index = 0; index < peer_count; index++) {
        (void)nb_node_add_discovery_peer(node, peer_hosts[index], peer_ports[index]);
    }
    rc = nb_service_create(node, service_name, &service);
    if (rc == NB_OK) {
        if (main_executor_threads > 0) {
            ntk_thread_pool_opts_t eopts;
            nb_service_method_options_t mopts = NB_SERVICE_METHOD_OPTIONS_INIT;
            memset(&eopts, 0, sizeof(eopts));
            eopts.thread_count = main_executor_threads;
            eopts.priority = NTK_THREAD_PRIO_NORMAL;
            eopts.auto_start = 1;
            main_executor_pool = ntk_thread_pool_create(&eopts);
            if (!main_executor_pool) {
                rc = NB_ERR_NOMEM;
            } else {
                main_executor.ctx = main_executor_pool;
                main_executor.submit = executor_submit;
                mopts.exec = NB_SERVICE_EXEC_EXECUTOR;
                mopts.executor = &main_executor;
                rc = nb_service_add_method_ex(service, method_name, order_get, &fast_options, &mopts);
            }
        } else {
            rc = use_worker ?
                nb_service_add_method_worker(service, method_name, order_get, &fast_options) :
                nb_service_add_method(service, method_name, order_get, &fast_options);
        }
    }
    if (rc == NB_OK && block_method_name && block_delay_ms > 0) {
        block_options.delay_ms = block_delay_ms;
        if (block_executor_threads > 0) {
            ntk_thread_pool_opts_t eopts;
            nb_service_method_options_t mopts = NB_SERVICE_METHOD_OPTIONS_INIT;
            memset(&eopts, 0, sizeof(eopts));
            eopts.thread_count = block_executor_threads;
            eopts.priority = NTK_THREAD_PRIO_NORMAL;
            eopts.auto_start = 1;
            block_executor_pool = ntk_thread_pool_create(&eopts);
            if (!block_executor_pool) {
                rc = NB_ERR_NOMEM;
            } else {
                block_executor.ctx = block_executor_pool;
                block_executor.submit = executor_submit;
                mopts.exec = NB_SERVICE_EXEC_EXECUTOR;
                mopts.executor = &block_executor;
                rc = nb_service_add_method_ex(
                    service, block_method_name, order_get, &block_options, &mopts);
            }
        } else if (block_worker) {
            rc = nb_service_add_method_worker(service, block_method_name, order_get, &block_options);
        } else {
            rc = nb_service_add_method(service, block_method_name, order_get, &block_options);
        }
    }
    if (rc != NB_OK) {
        goto done;
    }

    nb_log_info("ready: service=%s method=%s block_method=%s transport=%s data_port=%u "
                 "discovery_port=%u handler_threads=%u handler_max_concurrency=%u "
                 "worker=%d main_executor_threads=%u block_worker=%d "
                 "block_executor_threads=%u block_delay_ms=%u",
                 service_name,
                 method_name,
                 block_method_name ? block_method_name : "-",
                 use_shm ? "shm" : (use_dds ? "dds" :
                     (nb_node_transport(node) == NB_TRANSPORT_TCP ? "tcp" : "udp")),
                 (unsigned)nb_node_data_port(node),
                 (unsigned)nb_node_discovery_port(node),
                 node_cfg.runtime.handler_threads,
                 node_cfg.runtime.handler_max_concurrency,
                 use_worker,
                 main_executor_threads,
                 block_worker,
                 block_executor_threads,
                 (unsigned)block_delay_ms);

    while (!g_stop) {
        ntk_sleep_ms(100);
    }

done:
    if (main_executor_pool) {
        ntk_thread_pool_destroy(main_executor_pool);
        main_executor_pool = NULL;
    }
    if (block_executor_pool) {
        ntk_thread_pool_destroy(block_executor_pool);
        block_executor_pool = NULL;
    }
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
