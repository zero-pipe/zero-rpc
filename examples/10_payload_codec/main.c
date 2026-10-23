/*
 * 10 Payload Codec: register an application codec and round-trip an object.
 *
 * The framework only transports bytes plus an encoding tag. This example uses
 * a tiny "id|name" text format to demonstrate encode/decode/release.
 */
#include <extensions.h>
#include <runtime.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct order {
    int id;
    char name[32];
} order_t;

static int text_encode(void *ctx, const void *obj, void **out_data, size_t *out_len)
{
    const order_t *order = (const order_t *)obj;
    char *buf;
    int n;
    (void)ctx;

    buf = (char *)malloc(64);
    if (!buf) {
        return NB_ERR_NOMEM;
    }
    n = snprintf(buf, 64, "%d|%s", order->id, order->name);
    if (n < 0 || n >= 64) {
        free(buf);
        return NB_ERR_TOOBIG;
    }
    *out_data = buf;
    *out_len = (size_t)n;
    return NB_OK;
}

static int text_decode(void *ctx, const void *data, size_t len, void **out_obj)
{
    const char *text = (const char *)data;
    const char *sep;
    order_t *order;
    size_t name_len;
    (void)ctx;

    sep = (const char *)memchr(text, '|', len);
    if (!sep) {
        return NB_ERR_BAD_MESSAGE;
    }
    order = (order_t *)calloc(1, sizeof(*order));
    if (!order) {
        return NB_ERR_NOMEM;
    }
    order->id = atoi(text);
    name_len = len - (size_t)(sep + 1 - text);
    if (name_len >= sizeof(order->name)) {
        free(order);
        return NB_ERR_TOOBIG;
    }
    memcpy(order->name, sep + 1, name_len);
    order->name[name_len] = '\0';
    *out_obj = order;
    return NB_OK;
}

static void text_release(void *ctx, void *obj)
{
    (void)ctx;
    free(obj);
}

static const nb_codec_t g_text_codec = {
    "order-text", 7u, text_encode, text_decode, text_release, NULL,
};

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    const nb_codec_t *codec;
    order_t order = { 1001, "keyboard" };
    order_t *decoded = NULL;
    void *bytes = NULL;
    size_t len = 0;
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "CodecExample";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_node_register_codec(node, &g_text_codec);
    }
    if (rc != NB_OK) {
        nb_log_error("node/codec setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    codec = nb_node_find_codec(node, 7u);
    if (!codec) {
        nb_log_error("codec not found by encoding");
        nb_node_destroy(node);
        return 1;
    }
    rc = codec->encode(codec->ctx, &order, &bytes, &len);
    if (rc == NB_OK) {
        nb_payload_t payload = { bytes, len, codec->encoding };
        nb_log_info("encoded payload: %.*s", (int)payload.len, (const char *)payload.data);
        rc = codec->decode(codec->ctx, payload.data, payload.len, (void **)&decoded);
    }
    if (rc == NB_OK) {
        nb_log_info("decoded order: id=%d name=%s", decoded->id, decoded->name);
    } else {
        nb_log_error("codec round-trip failed: %d (%s)", rc, nb_error_name(rc));
    }

    if (decoded) {
        codec->release(codec->ctx, decoded);
    }
    if (bytes) {
        codec->release(codec->ctx, bytes);
    }
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
