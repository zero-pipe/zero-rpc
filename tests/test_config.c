#include <config.h>

#define RPC_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            rpc_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                            \
        }                                                          \
    } while (0)

static void test_parse(void) {
    const char *json =
        "{"
        "\"name\":\"n1\",\"transport\":\"tcp\",\"data_port\":9000,"
        "\"io_threads\":2,\"handler_threads\":3,\"handler_max_concurrency\":64,"
        "\"mode\":\"static\",\"frag_bytes\":1024,\"max_msg_bytes\":4194304,"
        "\"discovery\":{\"broadcast\":\"127.0.0.1:41953\",\"hello_interval_ms\":500,"
        "\"node_ttl_ms\":9000,"
        "\"peers\":[\"10.0.0.1:41953\"]},"
        "\"routes\":[{\"service\":\"echo\",\"endpoint\":\"tcp://10.0.0.2:9001\"}]"
        "}";
    config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load(json, &cfg) == OK);
    CHECK(strcmp(cfg.name, "n1") == 0);
    CHECK(cfg.node.transport == TRANSPORT_TCP);
    CHECK(cfg.node.mode == MODE_STATIC);
    CHECK(cfg.node.data_port == 9000);
    CHECK(cfg.node.io_threads == 2);
    CHECK(cfg.node.handler_threads == 3);
    CHECK(cfg.node.handler_max_concurrency == 64);
    CHECK(cfg.node.frag_bytes == 1024);
    CHECK(cfg.node.max_msg_bytes == 4194304);
    CHECK(strcmp(cfg.discovery_host, "127.0.0.1") == 0);
    CHECK(cfg.node.discovery_port == 41953);
    CHECK(cfg.node.hello_interval_ms == 500);
    CHECK(cfg.peer_count == 1);
    CHECK(strcmp(cfg.peers[0].ip, "10.0.0.1") == 0);
    CHECK(cfg.peers[0].port == 41953);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].service, "echo") == 0);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://10.0.0.2:9001") == 0);
}

static void test_defaults(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{}", &cfg) == OK);
    CHECK(cfg.node.transport == TRANSPORT_UDP);
    CHECK(cfg.node.mode == MODE_MESH);
    CHECK(strcmp(cfg.name, "rpc") == 0);
    CHECK(strcmp(cfg.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.discovery_host, "255.255.255.255") == 0);
}

static void test_bad_json(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{ not json", &cfg) == ERR_INVALID);
    CHECK(config_load(NULL, &cfg) == ERR_INVALID);
}

static void test_endpoint_model(void) {
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

static void test_binding_config(void) {
    const char *json =
        "{\"name\":\"dual\",\"mode\":\"static\","
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
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://127.0.0.1:9000") == 0);
}

/* 新的扁平格式：顶层字段 + URI 端点 + 字符串 peer/binding。 */
static void test_flat_format(void) {
    const char *json =
        "{"
        "\"name\":\"flat\",\"mode\":\"static\",\"io_threads\":4,"
        "\"bindings\":[\"tcp://127.0.0.1:5000\",\"udp://127.0.0.1:5001\"],"
        "\"discovery\":{\"broadcast\":\"127.0.0.1:42000\","
        "\"peers\":[\"10.0.0.1:42000\",\"10.0.0.2:42001\"]},"
        "\"routes\":[{\"service\":\"Foo\",\"endpoint\":\"tcp://10.0.0.9:6000\"}]"
        "}";
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load(json, &cfg) == OK);
    CHECK(strcmp(cfg.name, "flat") == 0);
    CHECK(cfg.node.mode == MODE_STATIC);
    CHECK(cfg.node.io_threads == 4);
    CHECK(cfg.binding_count == 2);
    CHECK(strcmp(cfg.binding_configs[0].name, "tcp") == 0);
    CHECK(strcmp(cfg.binding_configs[0].endpoint, "tcp://127.0.0.1:5000") == 0);
    CHECK(strcmp(cfg.binding_configs[1].name, "udp") == 0);
    CHECK(strcmp(cfg.discovery_host, "127.0.0.1") == 0);
    CHECK(cfg.node.discovery_port == 42000);
    CHECK(cfg.peer_count == 2);
    CHECK(strcmp(cfg.peers[0].ip, "10.0.0.1") == 0);
    CHECK(cfg.peers[0].port == 42000);
    CHECK(strcmp(cfg.peers[1].ip, "10.0.0.2") == 0);
    CHECK(cfg.peers[1].port == 42001);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].service, "Foo") == 0);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://10.0.0.9:6000") == 0);
}

static void test_rtcp_config(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{\"name\":\"r\",\"rtcp_interval_ms\":2000}", &cfg) == OK);
    CHECK(cfg.node.rtcp_interval_ms == 2000);
    CHECK(cfg.node.rtcp_disabled == 0);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{\"name\":\"r\",\"rtcp\":false}", &cfg) == OK);
    CHECK(cfg.node.rtcp_disabled == 1);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(config_load("{\"name\":\"r\",\"rtcp\":true}", &cfg) == OK);
    CHECK(cfg.node.rtcp_disabled == 0);
}

int main(void) {
    test_parse();
    test_defaults();
    test_bad_json();
    test_endpoint_model();
    test_binding_config();
    test_flat_format();
    test_rtcp_config();
    if (g_fail) {
        rpc_log_error("config: FAIL");
        return 1;
    }
    rpc_log_info("config: OK");
    return 0;
}
