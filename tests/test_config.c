#include <config.h>

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

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
    nb_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load(json, &cfg) == NB_OK);
    CHECK(strcmp(cfg.name, "nested") == 0);
    CHECK(cfg.node.transport.transport == NB_TRANSPORT_TCP);
    CHECK(cfg.node.mode == NB_MODE_STATIC);
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
    CHECK(cfg.node.log.level == NB_NODE_LOG_DEBUG);
    CHECK(strcmp(cfg.node.log.file, "rpc.log") == 0);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].service, "echo") == 0);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://10.0.0.2:9001") == 0);
}

static void test_defaults(void)
{
    nb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load("{}", &cfg) == NB_OK);
    CHECK(cfg.node.transport.transport == NB_TRANSPORT_UDP);
    CHECK(cfg.node.mode == NB_MODE_MESH);
    CHECK(strcmp(cfg.name, "rpc") == 0);
    CHECK(strcmp(cfg.node.transport.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.node.discovery.host, "255.255.255.255") == 0);
    CHECK(cfg.node.metrics.sample_every == 1);
    CHECK(cfg.node.log.level == NB_NODE_LOG_INFO);
    CHECK(cfg.binding_count == 0);
    CHECK(cfg.peer_count == 0);
    CHECK(cfg.route_count == 0);
}

static void test_bad_json(void)
{
    nb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load("{ not json", &cfg) == NB_ERR_INVALID);
    CHECK(nb_config_load(NULL, &cfg) == NB_ERR_INVALID);
}

static void test_strict_validation(void)
{
    nb_config_t cfg;
    static const char *bad[] = {
        "{\"mode\":\"foo\"}",
        "{\"name\":42}",
        "{\"routes\":[{\"service\":\"x\"}]}",
        "{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}",
        "{\"discovery\":[]}",
        "{\"discovery\":{\"broadcast\":\"bad\"}}",
        "{\"discovery\":{\"peers\":[\"bad\"]}}",
        "{\"runtime\":[]}",
        "{\"runtime\":{\"io_threads\":\"2\"}}",
        "{\"runtime\":{\"io_threads\":2000}}",
        "{\"transport\":[]}",
        "{\"transport\":{\"kind\":\"sctp\"}}",
        "{\"transport\":{\"data_port\":70000}}",
        "{\"transport\":{\"frag_bytes\":70000}}",
        "{\"transport\":{\"rtcp\":\"no\"}}",
        "{\"transport\":{\"bindings\":\"tcp://127.0.0.1:1\"}}",
        "{\"transport\":{\"bindings\":[\"not-a-uri\"]}}",
        "{\"metrics\":\"off\"}",
        "{\"log\":{\"level\":\"nope\"}}",
        "{\"discovery\":{\"mode\":\"foo\"}}",
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&cfg, 0, sizeof(cfg));
        if (nb_config_load(bad[i], &cfg) != NB_ERR_INVALID) {
            nb_log_error("strict config accepted invalid case %lu: %s",
                          (unsigned long)i,
                          bad[i]);
            g_fail = 1;
        }
        CHECK(cfg.name[0] == '\0');
    }
}

static void test_endpoint_model(void)
{
    nb_endpoint_t endpoint;
    char text[128];
    CHECK(nb_endpoint_parse("tcp://127.0.0.1:8080", &endpoint) == NB_OK);
    CHECK(endpoint.transport == NB_TRANSPORT_TCP);
    CHECK(endpoint.port == 8080);
    CHECK(nb_endpoint_format(&endpoint, text, sizeof(text)) == NB_OK);
    CHECK(strcmp(text, "tcp://127.0.0.1:8080") == 0);
    CHECK(nb_endpoint_parse("dds://127.0.0.1:7400", &endpoint) == NB_OK);
    CHECK(endpoint.transport == NB_TRANSPORT_CUSTOM);
}

static void test_binding_config(void)
{
    const char *json = "{\"name\":\"dual\",\"mode\":\"static\","
                       "\"transport\":{\"bindings\":["
                       "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
                       "{\"name\":\"udp\",\"endpoint\":\"udp://127.0.0.1:8081\"}]},"
                       "\"routes\":[{\"service\":\"order\","
                       "\"endpoint\":\"tcp://127.0.0.1:9000\"}]}";
    nb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load(json, &cfg) == NB_OK);
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
    nb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load(json, &cfg) == NB_OK);
    CHECK(cfg.binding_count == 2);
    CHECK(strcmp(cfg.binding_configs[0].name, "tcp") == 0);
    CHECK(strcmp(cfg.binding_configs[1].name, "udp") == 0);
    CHECK(strcmp(cfg.binding_configs[1].endpoint, "udp://127.0.0.1:8081") == 0);
}

static void test_rtcp_config(void)
{
    nb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load("{\"name\":\"r\",\"transport\":{\"rtcp_interval_ms\":2000}}", &cfg) == NB_OK);
    CHECK(cfg.node.transport.rtcp_interval_ms == 2000);
    CHECK(cfg.node.transport.rtcp_disabled == 0);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load("{\"name\":\"r\",\"transport\":{\"rtcp\":false}}", &cfg) == NB_OK);
    CHECK(cfg.node.transport.rtcp_disabled == 1);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load("{\"name\":\"r\",\"transport\":{\"rtcp\":true}}", &cfg) == NB_OK);
    CHECK(cfg.node.transport.rtcp_disabled == 0);
}

static void test_structured_errors(void)
{
    nb_config_t cfg;
    nb_config_error_t err;

    memset(&cfg, 0, sizeof(cfg));
    memset(&err, 0, sizeof(err));
    CHECK(nb_config_load_ex("{\"transport\":{\"frag_bytes\":70000}}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "transport.frag_bytes") == 0);
    CHECK(err.reason[0] != '\0');

    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load_ex("{\"mode\":\"nope\"}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "mode") == 0);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load_ex("{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}", &cfg, &err) ==
          NB_ERR_INVALID);
    CHECK(strcmp(err.path, "routes.endpoint") == 0);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_config_load_ex("{ not json", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "") == 0);
}

static void test_defaults_api(void)
{
    nb_node_config_t cfg;
    nb_node_config_default(&cfg);
    CHECK(strcmp(cfg.name, "rpc") == 0);
    CHECK(cfg.mode == NB_MODE_MESH);
    CHECK(cfg.transport.transport == NB_TRANSPORT_UDP);
    CHECK(strcmp(cfg.transport.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.discovery.host, "255.255.255.255") == 0);
    CHECK(cfg.metrics.sample_every == 1);
    CHECK(cfg.log.level == NB_NODE_LOG_INFO);
}

static void test_log_switch(void)
{
    CHECK(nb_log_enabled() == 1);
    nb_log_configure(0, NB_NODE_LOG_ERROR);
    CHECK(nb_log_enabled() == 0);
    nb_log_configure(1, NB_NODE_LOG_INFO);
    CHECK(nb_log_enabled() == 1);
}

int main(void)
{
    test_nested();
    test_defaults();
    test_bad_json();
    test_strict_validation();
    test_structured_errors();
    test_endpoint_model();
    test_binding_config();
    test_nested_bindings();
    test_rtcp_config();
    test_defaults_api();
    test_log_switch();
    if (g_fail) {
        nb_log_error("config: FAIL");
        return 1;
    }
    nb_log_info("config: NB_OK");
    return 0;
}
