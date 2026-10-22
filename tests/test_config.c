#include <config.h>

#define RPC_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            rpc_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

/* 旧扁平格式：顶层字段 + URI 端点 + 字符串 peer/binding。 */
static void test_legacy_flat(void)
{
    const char *json = "{"
                       "\"name\":\"n1\",\"transport\":\"tcp\",\"data_port\":9000,"
                       "\"io_threads\":2,\"handler_threads\":3,\"handler_max_concurrency\":64,"
                       "\"mode\":\"static\",\"frag_bytes\":1024,\"max_msg_bytes\":4194304,"
                       "\"discovery\":{\"broadcast\":\"127.0.0.1:41953\",\"hello_interval_ms\":500,"
                       "\"node_ttl_ms\":9000,"
                       "\"peers\":[\"10.0.0.1:41953\"]},"
                       "\"bindings\":[\"tcp://127.0.0.1:8080\"],"
                       "\"routes\":[{\"service\":\"echo\",\"endpoint\":\"tcp://10.0.0.2:9001\"}]"
                       "}";
    config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load(json, &cfg) == OK);
    CHECK(strcmp(cfg.name, "n1") == 0);
    CHECK(cfg.node.name == cfg.name);
    CHECK(cfg.node.transport.transport == TRANSPORT_TCP);
    CHECK(cfg.node.mode == MODE_STATIC);
    CHECK(cfg.node.transport.data_port == 9000);
    CHECK(cfg.node.runtime.io_threads == 2);
    CHECK(cfg.node.runtime.handler_threads == 3);
    CHECK(cfg.node.runtime.handler_max_concurrency == 64);
    CHECK(cfg.node.transport.frag_bytes == 1024);
    CHECK(cfg.node.transport.max_msg_bytes == 4194304);
    CHECK(strcmp(cfg.discovery_host, "127.0.0.1") == 0);
    CHECK(cfg.node.discovery.host == cfg.discovery_host);
    CHECK(cfg.node.discovery.port == 41953);
    CHECK(cfg.node.discovery.hello_interval_ms == 500);
    CHECK(cfg.binding_count == 1);
    CHECK(strcmp(cfg.binding_configs[0].name, "tcp") == 0);
    CHECK(strcmp(cfg.binding_configs[0].endpoint, "tcp://127.0.0.1:8080") == 0);
    CHECK(cfg.peer_count == 1);
    CHECK(strcmp(cfg.peers[0].ip, "10.0.0.1") == 0);
    CHECK(cfg.peers[0].port == 41953);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].service, "echo") == 0);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://10.0.0.2:9001") == 0);
}

/* 新格式：按模块拆成子对象。 */
static void test_nested(void)
{
    const char *json = "{"
                       "\"name\":\"nested\",\"mode\":\"static\","
                       "\"runtime\":{\"io_threads\":2,\"handler_threads\":3,"
                       "\"handler_max_concurrency\":64,\"max_inflight\":100,"
                       "\"max_streams\":8,\"peer_rate_limit_pps\":10},"
                       "\"transport\":{\"kind\":\"tcp\",\"bind_host\":\"127.0.0.1\","
                       "\"data_port\":9000,\"frag_bytes\":1024,\"max_msg_bytes\":4194304,"
                       "\"backpressure_bytes\":8192,\"rtcp_interval_ms\":2000,\"rtcp\":false,"
                       "\"bindings\":[\"tcp://127.0.0.1:9000\"]},"
                       "\"discovery\":{\"mode\":\"static\",\"broadcast\":\"127.0.0.1:41953\","
                       "\"hello_interval_ms\":500,\"node_ttl_ms\":9000,"
                       "\"peers\":[\"10.0.0.1:41953\"]},"
                       "\"metrics\":{\"disabled\":true,\"latency_disabled\":true,"
                       "\"sample_every\":5,\"per_peer_disabled\":true},"
                       "\"log\":{\"disabled\":true,\"level\":\"debug\",\"file\":\"rpc.log\"},"
                       "\"routes\":[{\"service\":\"echo\",\"endpoint\":\"tcp://10.0.0.2:9001\"}]"
                       "}";
    config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load(json, &cfg) == OK);
    CHECK(strcmp(cfg.name, "nested") == 0);
    CHECK(cfg.node.transport.transport == TRANSPORT_TCP);
    CHECK(cfg.node.mode == MODE_STATIC);
    CHECK(strcmp(cfg.node.transport.bind_host, "127.0.0.1") == 0);
    CHECK(cfg.node.transport.data_port == 9000);
    CHECK(cfg.node.transport.frag_bytes == 1024);
    CHECK(cfg.node.transport.max_msg_bytes == 4194304);
    CHECK(cfg.node.transport.backpressure_bytes == 8192);
    CHECK(cfg.node.transport.rtcp_interval_ms == 2000);
    CHECK(cfg.node.transport.rtcp_disabled == 1);
    CHECK(cfg.node.runtime.io_threads == 2);
    CHECK(cfg.node.runtime.handler_threads == 3);
    CHECK(cfg.node.runtime.handler_max_concurrency == 64);
    CHECK(cfg.node.runtime.max_inflight == 100);
    CHECK(cfg.node.runtime.max_streams == 8);
    CHECK(cfg.node.runtime.peer_rate_limit_pps == 10);
    CHECK(cfg.node.discovery.disabled == 1);
    CHECK(strcmp(cfg.discovery_host, "127.0.0.1") == 0);
    CHECK(cfg.node.discovery.port == 41953);
    CHECK(cfg.node.discovery.hello_interval_ms == 500);
    CHECK(cfg.node.discovery.node_ttl_ms == 9000);
    CHECK(cfg.peer_count == 1);
    CHECK(cfg.node.metrics.disabled == 1);
    CHECK(cfg.node.metrics.latency_disabled == 1);
    CHECK(cfg.node.metrics.sample_every == 5);
    CHECK(cfg.node.metrics.per_peer_disabled == 1);
    CHECK(cfg.node.log.disabled == 1);
    CHECK(cfg.node.log.level == NODE_LOG_DEBUG);
    CHECK(strcmp(cfg.node.log.file, "rpc.log") == 0);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].service, "echo") == 0);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://10.0.0.2:9001") == 0);
}

static void test_defaults(void)
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{}", &cfg) == OK);
    CHECK(cfg.node.transport.transport == TRANSPORT_UDP);
    CHECK(cfg.node.mode == MODE_MESH);
    CHECK(strcmp(cfg.name, "rpc") == 0);
    CHECK(strcmp(cfg.node.transport.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.node.discovery.host, "255.255.255.255") == 0);
    CHECK(cfg.node.metrics.sample_every == 1);
    CHECK(cfg.node.log.level == NODE_LOG_INFO);
    CHECK(cfg.binding_count == 0);
    CHECK(cfg.peer_count == 0);
    CHECK(cfg.route_count == 0);
}

static void test_bad_json(void)
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{ not json", &cfg) == ERR_INVALID);
    CHECK(config_load(NULL, &cfg) == ERR_INVALID);
}

static void test_strict_validation(void)
{
    config_t cfg;
    static const char *bad[] = {
        /* 旧扁平格式 */
        "{\"data_port\":\"9000\"}",
        "{\"data_port\":70000}",
        "{\"transport\":\"sctp\"}",
        "{\"mode\":\"foo\"}",
        "{\"frag_bytes\":70000}",
        "{\"name\":42}",
        "{\"bindings\":\"tcp://127.0.0.1:1\"}",
        "{\"bindings\":[\"not-a-uri\"]}",
        "{\"routes\":[{\"service\":\"x\"}]}",
        "{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}",
        "{\"discovery\":[]}",
        "{\"discovery\":{\"broadcast\":\"bad\"}}",
        "{\"discovery\":{\"peers\":[\"bad\"]}}",
        "{\"rtcp\":\"no\"}",
        /* 新子结构格式 */
        "{\"runtime\":[]}",
        "{\"runtime\":{\"io_threads\":\"2\"}}",
        "{\"runtime\":{\"io_threads\":2000}}",
        "{\"transport\":[]}",
        "{\"transport\":{\"kind\":\"sctp\"}}",
        "{\"transport\":{\"data_port\":70000}}",
        "{\"transport\":{\"bindings\":\"tcp://127.0.0.1:1\"}}",
        "{\"transport\":{\"bindings\":[\"not-a-uri\"]}}",
        "{\"metrics\":\"off\"}",
        "{\"log\":{\"level\":\"nope\"}}",
        "{\"discovery\":{\"mode\":\"foo\"}}",
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&cfg, 0, sizeof(cfg));
        if (config_load(bad[i], &cfg) != ERR_INVALID) {
            rpc_log_error("strict config accepted invalid case %lu: %s",
                          (unsigned long)i,
                          bad[i]);
            g_fail = 1;
        }
        CHECK(cfg.name[0] == '\0');
    }
}

static void test_endpoint_model(void)
{
    endpoint_t endpoint;
    char text[128];
    CHECK(endpoint_parse("tcp://127.0.0.1:8080", &endpoint) == OK);
    CHECK(endpoint.transport == TRANSPORT_TCP);
    CHECK(endpoint.port == 8080);
    CHECK(endpoint_format(&endpoint, text, sizeof(text)) == OK);
    CHECK(strcmp(text, "tcp://127.0.0.1:8080") == 0);
    CHECK(endpoint_parse("dds://127.0.0.1:7400", &endpoint) == OK);
    CHECK(endpoint.transport == TRANSPORT_CUSTOM);
}

static void test_binding_config(void)
{
    const char *json = "{\"name\":\"dual\",\"mode\":\"static\","
                       "\"bindings\":["
                       "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
                       "{\"name\":\"udp\",\"endpoint\":\"udp://127.0.0.1:8081\"}],"
                       "\"routes\":[{\"service\":\"order\","
                       "\"endpoint\":\"tcp://127.0.0.1:9000\"}]}";
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load(json, &cfg) == OK);
    CHECK(cfg.binding_count == 2);
    CHECK(strcmp(cfg.binding_configs[0].name, "tcp") == 0);
    CHECK(strcmp(cfg.binding_configs[1].endpoint, "udp://127.0.0.1:8081") == 0);
    CHECK(cfg.node.transport.binding_count == 2);
    CHECK(cfg.node.transport.bindings == cfg.binding_configs);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://127.0.0.1:9000") == 0);
}

static void test_nested_bindings(void)
{
    const char *json = "{\"transport\":{\"bindings\":["
                       "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
                       "\"udp://127.0.0.1:8081\"]}}";
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load(json, &cfg) == OK);
    CHECK(cfg.binding_count == 2);
    CHECK(strcmp(cfg.binding_configs[0].name, "tcp") == 0);
    CHECK(strcmp(cfg.binding_configs[1].name, "udp") == 0);
    CHECK(strcmp(cfg.binding_configs[1].endpoint, "udp://127.0.0.1:8081") == 0);
}

static void test_rtcp_config(void)
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{\"name\":\"r\",\"rtcp_interval_ms\":2000}", &cfg) == OK);
    CHECK(cfg.node.transport.rtcp_interval_ms == 2000);
    CHECK(cfg.node.transport.rtcp_disabled == 0);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{\"name\":\"r\",\"rtcp\":false}", &cfg) == OK);
    CHECK(cfg.node.transport.rtcp_disabled == 1);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{\"name\":\"r\",\"rtcp\":true}", &cfg) == OK);
    CHECK(cfg.node.transport.rtcp_disabled == 0);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{\"transport\":{\"rtcp\":false}}", &cfg) == OK);
    CHECK(cfg.node.transport.rtcp_disabled == 1);
}

static void test_defaults_api(void)
{
    node_config_t cfg;
    node_config_default(&cfg);
    CHECK(strcmp(cfg.name, "rpc") == 0);
    CHECK(cfg.mode == MODE_MESH);
    CHECK(cfg.transport.transport == TRANSPORT_UDP);
    CHECK(strcmp(cfg.transport.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.discovery.host, "255.255.255.255") == 0);
    CHECK(cfg.metrics.sample_every == 1);
    CHECK(cfg.log.level == NODE_LOG_INFO);
}

int main(void)
{
    test_legacy_flat();
    test_nested();
    test_defaults();
    test_bad_json();
    test_strict_validation();
    test_endpoint_model();
    test_binding_config();
    test_nested_bindings();
    test_rtcp_config();
    test_defaults_api();
    if (g_fail) {
        rpc_log_error("config: FAIL");
        return 1;
    }
    rpc_log_info("config: OK");
    return 0;
}
