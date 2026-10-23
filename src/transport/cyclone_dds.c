/*
 * Cyclone DDS transport provider.
 *
 * The provider carries the framework's encoded v2 frame as an opaque octet
 * sample. Topics are derived from the method name:
 *   request  -> <method>_req
 *   response -> <method>_res
 *   event    -> <method>_evt
 *
 * Service registration calls the provider subscribe hook, which creates the
 * request/event readers and the response writer. Client requests lazily create
 * the request writer and response reader. Events are one-way publishes.
 */
#include "transport_internal.h"

#include "envelope.h"
#include "util.h"

#include <log.h>

#include <dds/dds.h>

#include "cy_dds_message.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_DDS_MAX_ENDPOINTS 32
#define NB_DDS_MAX_SAMPLES 32
#define NB_DDS_TOPIC_MAX 160

typedef struct nb_dds_instance nb_dds_instance_t;

typedef struct nb_dds_endpoint {
    int used;
    char method[NB_METHOD_NAME_MAX];
    char topic_req[NB_DDS_TOPIC_MAX];
    char topic_res[NB_DDS_TOPIC_MAX];
    char topic_evt[NB_DDS_TOPIC_MAX];
    dds_entity_t topic_req_ent;
    dds_entity_t topic_res_ent;
    dds_entity_t topic_evt_ent;
    dds_entity_t wr_req;
    dds_entity_t rd_req;
    dds_entity_t wr_res;
    dds_entity_t rd_res;
    dds_entity_t wr_evt;
    dds_entity_t rd_evt;
    dds_listener_t *listener;
    int have_responder;
    int have_requester;
    int have_publisher;
    nb_dds_instance_t *owner;
} nb_dds_endpoint_t;

struct nb_dds_instance {
    nb_transport_provider_options_t options;
    dds_entity_t participant;
    ntk_mutex *lock;
    nb_dds_endpoint_t endpoints[NB_DDS_MAX_ENDPOINTS];
    size_t endpoint_count;
};

static int nb_dds_status(int status, const char *what)
{
    if (status >= 0) {
        return NB_OK;
    }
    nb_log_error("cyclone dds %s failed: %s (%d)",
                 what ? what : "call",
                 dds_strretcode(status),
                 status);
    return NB_ERR_IO;
}

static void nb_dds_sanitize(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    size_t j = 0;

    if (!out || cap == 0) {
        return;
    }
    if (!in) {
        out[0] = '\0';
        return;
    }
    if (in[0] >= '0' && in[0] <= '9') {
        out[j++] = '_';
    }
    for (i = 0; in[i] != '\0' && j + 1 < cap; ++i) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

/* "Service.method" -> "method". A route without a dot is used as-is. */
static void nb_dds_method_from_route(const char *route, char *out, size_t cap)
{
    const char *dot;
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!route) {
        return;
    }
    dot = strchr(route, '.');
    nb_dds_sanitize(dot ? dot + 1 : route, out, cap);
}

static void nb_dds_endpoint_topics(nb_dds_endpoint_t *ep)
{
    char suffix[8];

    nb_dds_sanitize(ep->method, ep->topic_req, sizeof(ep->topic_req));
    snprintf(suffix, sizeof(suffix), "_req");
    strncat(ep->topic_req, suffix, sizeof(ep->topic_req) - strlen(ep->topic_req) - 1);

    nb_dds_sanitize(ep->method, ep->topic_res, sizeof(ep->topic_res));
    snprintf(suffix, sizeof(suffix), "_res");
    strncat(ep->topic_res, suffix, sizeof(ep->topic_res) - strlen(ep->topic_res) - 1);

    nb_dds_sanitize(ep->method, ep->topic_evt, sizeof(ep->topic_evt));
    snprintf(suffix, sizeof(suffix), "_evt");
    strncat(ep->topic_evt, suffix, sizeof(ep->topic_evt) - strlen(ep->topic_evt) - 1);
}

static void nb_dds_endpoint_init(nb_dds_endpoint_t *ep, nb_dds_instance_t *owner, const char *method)
{
    memset(ep, 0, sizeof(*ep));
    ep->used = 1;
    ep->owner = owner;
    nb_copy_str(ep->method, sizeof(ep->method), method, "topic");
    ep->topic_req_ent = DDS_ENTITY_NIL;
    ep->topic_res_ent = DDS_ENTITY_NIL;
    ep->topic_evt_ent = DDS_ENTITY_NIL;
    ep->wr_req = DDS_ENTITY_NIL;
    ep->rd_req = DDS_ENTITY_NIL;
    ep->wr_res = DDS_ENTITY_NIL;
    ep->rd_res = DDS_ENTITY_NIL;
    ep->wr_evt = DDS_ENTITY_NIL;
    ep->rd_evt = DDS_ENTITY_NIL;
    nb_dds_endpoint_topics(ep);
}

static dds_entity_t nb_dds_make_topic(nb_dds_instance_t *inst, const char *name)
{
    dds_entity_t topic = dds_create_topic(inst->participant, &dds_Message_desc, name, NULL, NULL);
    if (topic < 0) {
        nb_log_error("cyclone dds topic create failed: %s (%d)", name, topic);
    }
    return topic;
}

static void nb_dds_on_data(dds_entity_t reader, void *arg)
{
    nb_dds_endpoint_t *ep = (nb_dds_endpoint_t *)arg;
    void *samples[NB_DDS_MAX_SAMPLES];
    dds_sample_info_t infos[NB_DDS_MAX_SAMPLES];
    dds_return_t count;
    int32_t i;

    if (!ep || !ep->owner) {
        return;
    }
    memset(samples, 0, sizeof(samples));
    memset(infos, 0, sizeof(infos));
    count = dds_take(reader, samples, infos, NB_DDS_MAX_SAMPLES, NB_DDS_MAX_SAMPLES);
    if (count <= 0) {
        return;
    }
    for (i = 0; i < count; ++i) {
        dds_Message *sample = (dds_Message *)samples[i];
        nb_envelope_t env;
        nb_transport_message_t msg;
        char route_buf[MAX_ROUTE];

        if (!infos[i].valid_data || !sample || !sample->payload._buffer || sample->payload._length == 0) {
            continue;
        }
        if (nb_envelope_decode(sample->payload._buffer, sample->payload._length, &env) != NB_OK) {
            continue;
        }
        memset(&msg, 0, sizeof(msg));
        msg.kind = env.kind;
        msg.flags = env.flags;
        msg.status = env.status;
        msg.request_id = env.request_id;
        msg.timeout_ms = env.timeout_ms;
        msg.metadata = env.metadata;
        msg.metadata_len = env.metadata_len;
        msg.status_msg = env.status_msg;
        msg.status_msg_len = env.status_msg_len;
        if (env.route && env.route_len < sizeof(route_buf)) {
            memcpy(route_buf, env.route, env.route_len);
            route_buf[env.route_len] = '\0';
            msg.route = route_buf;
        }
        msg.payload.data = env.payload;
        msg.payload.len = env.payload_len;
        msg.payload.encoding = env.encoding;
        if (env.stream_flags != 0 || env.stream_id != 0) {
            msg.has_stream = 1;
            msg.stream.flags = env.stream_flags;
            msg.stream.stream_id = env.stream_id;
            msg.stream.offset = env.stream_offset;
            msg.stream.total_size = env.stream_total_size;
        }
        ep->owner->options.on_receive(
            ep->owner->options.receiver, ep, "dds", 0, &msg);
    }
    dds_return_loan(reader, samples, count);
}

static nb_dds_endpoint_t *nb_dds_endpoint_ensure(nb_dds_instance_t *inst, const char *method, int roles)
{
    nb_dds_endpoint_t *ep = NULL;
    size_t i;

    if (!inst || !method || !method[0]) {
        return NULL;
    }
    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        if (inst->endpoints[i].used && strcmp(inst->endpoints[i].method, method) == 0) {
            ep = &inst->endpoints[i];
            break;
        }
    }
    if (!ep) {
        if (inst->endpoint_count >= NB_DDS_MAX_ENDPOINTS) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep = &inst->endpoints[inst->endpoint_count++];
        nb_dds_endpoint_init(ep, inst, method);
        ep->topic_req_ent = nb_dds_make_topic(inst, ep->topic_req);
        ep->topic_res_ent = nb_dds_make_topic(inst, ep->topic_res);
        ep->topic_evt_ent = nb_dds_make_topic(inst, ep->topic_evt);
        if (ep->topic_req_ent < 0 || ep->topic_res_ent < 0 || ep->topic_evt_ent < 0) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
    }
    if ((roles & 1) && !ep->have_responder) { /* responder */
        ep->listener = dds_create_listener(ep);
        if (!ep->listener) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        dds_lset_data_available(ep->listener, nb_dds_on_data);
        ep->rd_req = dds_create_reader(inst->participant, ep->topic_req_ent, NULL, ep->listener);
        ep->rd_evt = dds_create_reader(inst->participant, ep->topic_evt_ent, NULL, ep->listener);
        ep->wr_res = dds_create_writer(inst->participant, ep->topic_res_ent, NULL, NULL);
        if (ep->rd_req < 0 || ep->rd_evt < 0 || ep->wr_res < 0) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_responder = 1;
    }
    if ((roles & 2) && !ep->have_requester) { /* requester */
        ep->wr_req = dds_create_writer(inst->participant, ep->topic_req_ent, NULL, NULL);
        if (!ep->listener) {
            ep->listener = dds_create_listener(ep);
            if (!ep->listener) {
                ntk_mutex_unlock(inst->lock);
                return NULL;
            }
            dds_lset_data_available(ep->listener, nb_dds_on_data);
        }
        ep->rd_res = dds_create_reader(inst->participant, ep->topic_res_ent, NULL, ep->listener);
        if (ep->wr_req < 0 || ep->rd_res < 0) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_requester = 1;
    }
    if ((roles & 4) && !ep->have_publisher) { /* publisher */
        ep->wr_evt = dds_create_writer(inst->participant, ep->topic_evt_ent, NULL, NULL);
        if (ep->wr_evt < 0) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_publisher = 1;
    }
    ntk_mutex_unlock(inst->lock);
    return ep;
}

static int nb_dds_publish(nb_dds_instance_t *inst, dds_entity_t writer, const nb_transport_message_t *message)
{
    nb_envelope_t env;
    dds_Message sample;
    uint8_t *wire;
    size_t len;
    int rc;

    if (writer == DDS_ENTITY_NIL || !message) {
        return NB_ERR_INVALID;
    }
    memset(&env, 0, sizeof(env));
    env.kind = message->kind;
    env.encoding = message->payload.encoding;
    env.flags = message->flags;
    env.status = message->status;
    env.request_id = message->request_id;
    env.timeout_ms = message->timeout_ms;
    env.route = message->route;
    env.metadata = message->metadata;
    env.metadata_len = message->metadata_len;
    env.status_msg = message->status_msg;
    env.status_msg_len = message->status_msg_len;
    env.payload = message->payload.data;
    env.payload_len = message->payload.len;
    if (message->has_stream) {
        env.stream_flags = (uint8_t)message->stream.flags;
        env.stream_id = message->stream.stream_id;
        env.stream_offset = message->stream.offset;
        env.stream_total_size = message->stream.total_size;
    }
    len = nb_envelope_size(&env);
    if (len == 0) {
        return NB_ERR_INVALID;
    }
    wire = (uint8_t *)malloc(len);
    if (!wire) {
        return NB_ERR_NOMEM;
    }
    if (nb_envelope_encode(wire, len, &env) != len) {
        free(wire);
        return NB_ERR_IO;
    }
    memset(&sample, 0, sizeof(sample));
    sample.payload._buffer = wire;
    sample.payload._length = (uint32_t)len;
    sample.payload._maximum = (uint32_t)len;
    sample.payload._release = false;
    rc = dds_write(writer, &sample);
    free(wire);
    (void)inst;
    return nb_dds_status(rc, "dds_write");
}

/* ---- provider ops ---- */

static int nb_dds_op_create(const nb_transport_provider_options_t *options, nb_transport_instance_t **out)
{
    nb_dds_instance_t *inst;
    const char *p;
    char domain_text[16];
    size_t n = 0;
    int domain = 0;

    if (!options || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    inst = (nb_dds_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) {
        return NB_ERR_NOMEM;
    }
    inst->options = *options;
    inst->participant = DDS_ENTITY_NIL;

    p = options->endpoint ? strstr(options->endpoint, "://") : NULL;
    if (p) {
        p += 3;
        while (p[n] && p[n] != ':' && n + 1 < sizeof(domain_text)) {
            domain_text[n] = p[n];
            n++;
        }
    }
    domain_text[n] = '\0';
    if (domain_text[0]) {
        domain = (int)strtol(domain_text, NULL, 10);
    }

    inst->participant = dds_create_participant(domain, NULL, NULL);
    if (inst->participant < 0) {
        nb_log_error("cyclone dds participant create failed: %s (%d)",
                     dds_strretcode(inst->participant),
                     inst->participant);
        free(inst);
        return NB_ERR_IO;
    }
    inst->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!inst->lock) {
        dds_delete(inst->participant);
        free(inst);
        return NB_ERR_NOMEM;
    }
    *out = (nb_transport_instance_t *)inst;
    return NB_OK;
}

static int nb_dds_op_start(nb_transport_instance_t *instance)
{
    return instance ? NB_OK : NB_ERR_INVALID;
}

static void nb_dds_op_stop(nb_transport_instance_t *instance)
{
    (void)instance;
}

static void nb_dds_op_destroy(nb_transport_instance_t *instance)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    size_t i;

    if (!inst) {
        return;
    }
    for (i = 0; i < inst->endpoint_count; ++i) {
        nb_dds_endpoint_t *ep = &inst->endpoints[i];
        if (!ep->used) {
            continue;
        }
        if (ep->rd_req != DDS_ENTITY_NIL) dds_delete(ep->rd_req);
        if (ep->rd_res != DDS_ENTITY_NIL) dds_delete(ep->rd_res);
        if (ep->rd_evt != DDS_ENTITY_NIL) dds_delete(ep->rd_evt);
        if (ep->wr_req != DDS_ENTITY_NIL) dds_delete(ep->wr_req);
        if (ep->wr_res != DDS_ENTITY_NIL) dds_delete(ep->wr_res);
        if (ep->wr_evt != DDS_ENTITY_NIL) dds_delete(ep->wr_evt);
        if (ep->listener) dds_delete_listener(ep->listener);
        if (ep->topic_req_ent != DDS_ENTITY_NIL) dds_delete(ep->topic_req_ent);
        if (ep->topic_res_ent != DDS_ENTITY_NIL) dds_delete(ep->topic_res_ent);
        if (ep->topic_evt_ent != DDS_ENTITY_NIL) dds_delete(ep->topic_evt_ent);
    }
    if (inst->participant != DDS_ENTITY_NIL) {
        dds_delete(inst->participant);
    }
    if (inst->lock) {
        ntk_mutex_destroy(inst->lock);
    }
    free(inst);
}

static int nb_dds_op_send(nb_transport_instance_t *instance,
                          const char *host,
                          uint16_t port,
                          const nb_transport_message_t *message)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    nb_dds_endpoint_t *ep;
    char method[NB_METHOD_NAME_MAX];
    int roles;
    (void)host;
    (void)port;

    if (!inst || !message) {
        return NB_ERR_INVALID;
    }
    nb_dds_method_from_route(message->route, method, sizeof(method));
    if (!method[0]) {
        return NB_ERR_INVALID;
    }
    if (message->kind == KIND_EVENT) {
        roles = 4;
        ep = nb_dds_endpoint_ensure(inst, method, roles);
        return ep ? nb_dds_publish(inst, ep->wr_evt, message) : NB_ERR_NOMEM;
    }
    roles = 2;
    ep = nb_dds_endpoint_ensure(inst, method, roles);
    return ep ? nb_dds_publish(inst, ep->wr_req, message) : NB_ERR_NOMEM;
}

static int nb_dds_op_reply(nb_transport_instance_t *instance,
                           void *reply_token,
                           const char *host,
                           uint16_t port,
                           const nb_transport_message_t *message)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    nb_dds_endpoint_t *ep = (nb_dds_endpoint_t *)reply_token;
    (void)host;
    (void)port;

    if (!inst || !ep || !message || ep->wr_res == DDS_ENTITY_NIL) {
        return NB_ERR_INVALID;
    }
    return nb_dds_publish(inst, ep->wr_res, message);
}

static int nb_dds_op_subscribe(nb_transport_instance_t *instance,
                               const char *service,
                               const char *method)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    (void)service;
    if (!inst || !method || !method[0]) {
        return NB_ERR_INVALID;
    }
    return nb_dds_endpoint_ensure(inst, method, 1) ? NB_OK : NB_ERR_NOMEM;
}

static void nb_dds_op_unsubscribe(nb_transport_instance_t *instance,
                                  const char *service,
                                  const char *method)
{
    (void)instance;
    (void)service;
    (void)method;
}

static const nb_transport_provider_ops_t nb_g_dds_ops = {
    nb_dds_op_create,
    nb_dds_op_start,
    nb_dds_op_stop,
    nb_dds_op_destroy,
    nb_dds_op_send,
    nb_dds_op_reply,
    nb_dds_op_subscribe,
    nb_dds_op_unsubscribe,
};

static const nb_transport_provider_t nb_g_dds_provider = {
    "dds",
    &nb_g_dds_ops,
};

const nb_transport_provider_t *nb_cyclone_dds_provider(void)
{
    return &nb_g_dds_provider;
}

int nb_cyclone_dds_register(void)
{
    if (nb_transport_provider_find("dds")) {
        return NB_OK;
    }
    return nb_transport_provider_register(&nb_g_dds_provider);
}
