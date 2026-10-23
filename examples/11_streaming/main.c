/*
 * 11 Streaming: upload chunks from a client and reply after NB_STREAM_LAST.
 *
 * Both nodes live in one process. The server binds TCP port 0 and the client
 * queries the assigned port, so no external process or fixed port is needed.
 */
#include <client.h>
#include <server.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdio.h>
#include <string.h>

typedef struct upload_state {
    char buf[256];
    size_t len;
    int chunks;
} upload_state_t;

typedef struct response_state {
    char buf[256];
    size_t len;
    int done;
    int status;
} response_state_t;

static void on_upload(nb_call_t *call, const nb_request_t *req, const nb_chunk_t *chunk, void *user)
{
    upload_state_t *st = (upload_state_t *)user;
    (void)req;

    st->chunks++;
    if (chunk->len > 0 && st->len + chunk->len <= sizeof(st->buf)) {
        memcpy(st->buf + st->len, chunk->data, chunk->len);
        st->len += chunk->len;
    }
    if (chunk->flags & NB_STREAM_LAST) {
        nb_log_info("server received %d chunks, %zu bytes", st->chunks, st->len);
        nb_reply_bytes(call, NB_OK, st->buf, st->len);
    }
}

static void on_response(int status, const nb_response_t *resp, void *user)
{
    response_state_t *st = (response_state_t *)user;
    st->status = status;
    st->done = 1;
    if (status == NB_OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
    }
}

static int wait_done(nb_node_t *client, response_state_t *st)
{
    int i;

    for (i = 0; i < 50 && !st->done; ++i) {
        (void)nb_node_spin_once(client, 100);
    }
    return st->done ? NB_OK : NB_ERR_TIMEOUT;
}

int main(void)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_stream_t *stream = NULL;
    upload_state_t upload;
    response_state_t response;
    nb_chunk_t chunk;
    char endpoint[64];
    int rc;

    memset(&upload, 0, sizeof(upload));
    memset(&response, 0, sizeof(response));

    nb_node_config_default(&scfg);
    scfg.name = "StreamServer";
    scfg.mode = NB_MODE_STATIC;
    scfg.transport.transport = NB_TRANSPORT_TCP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.transport.data_port = 0;
    scfg.runtime.io_threads = 1;

    nb_node_config_default(&ccfg);
    ccfg.name = "StreamClient";
    ccfg.mode = NB_MODE_STATIC;
    ccfg.transport.transport = NB_TRANSPORT_TCP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.transport.data_port = 0;
    ccfg.runtime.io_threads = 1;

    rc = nb_node_create(&scfg, &server);
    if (rc == NB_OK) {
        rc = nb_service_create(server, "UploadService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_stream_method(svc, "chunk", on_upload, &upload);
    }
    if (rc == NB_OK) {
        rc = nb_node_create(&ccfg, &client);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(client, NULL, &proxy);
    }
    if (rc == NB_OK) {
        snprintf(endpoint,
                 sizeof(endpoint),
                 "tcp://127.0.0.1:%u",
                 (unsigned)nb_node_data_port(server));
        rc = nb_proxy_set_endpoint_uri(proxy, endpoint);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_stream_open(
            proxy, "UploadService", "chunk", on_response, &response, 3000, &stream);
    }
    if (rc != NB_OK) {
        nb_log_error("stream setup failed: %d (%s)", rc, nb_error_name(rc));
        goto done;
    }

    memset(&chunk, 0, sizeof(chunk));
    chunk.data = "hello ";
    chunk.len = 6;
    chunk.offset = 0;
    chunk.flags = NB_STREAM_FIRST;
    rc = nb_proxy_stream_send(stream, &chunk);

    if (rc == NB_OK) {
        chunk.data = "streaming ";
        chunk.len = 10;
        chunk.offset = 6;
        chunk.flags = 0;
        rc = nb_proxy_stream_send(stream, &chunk);
    }
    if (rc == NB_OK) {
        chunk.data = "world";
        chunk.len = 5;
        chunk.offset = 16;
        chunk.flags = NB_STREAM_LAST;
        rc = nb_proxy_stream_send(stream, &chunk);
    }
    if (rc == NB_OK) {
        rc = wait_done(client, &response);
    }
    if (rc == NB_OK && response.status == NB_OK) {
        nb_log_info("client received: %.*s", (int)response.len, response.buf);
    } else {
        nb_log_error("stream call failed: rc=%d status=%d", rc, response.status);
        if (rc == NB_OK) {
            rc = response.status;
        }
    }

done:
    if (stream) {
        (void)nb_proxy_stream_destroy(stream);
    }
    nb_proxy_destroy(proxy);
    nb_node_destroy(client);
    nb_service_destroy(svc);
    nb_node_destroy(server);
    return rc == NB_OK ? 0 : 1;
}
