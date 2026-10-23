/*
 * 14 DDS pub/sub: one process, one DDS domain, one subscriber and one publisher.
 *
 * The subscriber is an ordinary service whose method names the topic. The
 * publisher is an ordinary proxy using the one-way event call. Topics are
 * derived by the DDS provider as <method>_evt.
 */
#include <client.h>
#include <server.h>
#include <dds.h>
#include <platform.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdio.h>
#include <string.h>

static volatile int g_received;
static char g_last[128];

static void on_chat(nb_call_t *call, const nb_request_t *req, void *user)
{
    size_t n = req->payload.len;
    (void)call;
    (void)user;
    if (n >= sizeof(g_last)) {
        n = sizeof(g_last) - 1;
    }
    memcpy(g_last, req->payload.data, n);
    g_last[n] = '\0';
    g_received++;
}

static int wait_for_receive(nb_node_t *node, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms && g_received == 0) {
        (void)nb_node_spin_once(node, 100);
        elapsed += 100;
    }
    return g_received > 0 ? NB_OK : NB_ERR_TIMEOUT;
}

int main(void)
{
    nb_binding_config_t binding;
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *sub = NULL;
    nb_proxy_t *pub = NULL;
    nb_payload_t payload;
    int rc;

    rc = nb_cyclone_dds_register();
    if (rc != NB_OK) {
        nb_log_error("dds register failed: %d (%s)", rc, nb_error_name(rc));
        return 1;
    }

    memset(&binding, 0, sizeof(binding));
    binding.name = "dds";
    binding.endpoint = "dds://0:1";
    binding.intra_process = NB_INTRA_ONLY;

    nb_node_config_default(&cfg);
    cfg.name = "DdsPubSub";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "TopicService", &sub);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(sub, "chat", on_chat, NULL);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &pub);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_endpoint_uri(pub, "dds://0:1");
    }
    if (rc != NB_OK) {
        nb_log_error("dds pub/sub setup failed: %d (%s)", rc, nb_error_name(rc));
        goto done;
    }

    payload.data = "hello dds pubsub";
    payload.len = strlen((const char *)payload.data);
    payload.encoding = 0;
    /*
     * Reliable volatile writers can drop samples before discovery completes.
     * Publish until the subscriber observes one or the deadline expires.
     */
    for (int i = 0; i < 30 && g_received == 0; ++i) {
        rc = nb_proxy_call_event(pub, "TopicService", "chat", &payload, NULL);
        if (rc != NB_OK) {
            break;
        }
        ntk_sleep_ms(100);
    }
    if (rc == NB_OK) {
        rc = wait_for_receive(node, 3000);
    }
    if (rc == NB_OK) {
        nb_log_info("subscriber received: %s", g_last);
    } else {
        nb_log_error("dds pub/sub failed: %d (%s)", rc, nb_error_name(rc));
    }

done:
    nb_proxy_destroy(pub);
    nb_service_destroy(sub);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
