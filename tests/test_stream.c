/* Streaming RPC：分片上传请求，服务端逐 chunk 回调并在 LAST 回复。 */
#include <client.h>
#include <server.h>
#include <platform.h>
#include <thread/sem.h>

#define RPC_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

typedef struct upload_state {
    int chunks;
    int first_seen;
    int last_seen;
    size_t bytes;
    char buf[256];
} upload_state_t;

static void on_upload(call_t *call, const request_t *req, const chunk_t *chunk,
                      void *user) {
    upload_state_t *st = (upload_state_t *)user;
    (void)req;
    st->chunks++;
    if (chunk->flags & STREAM_FIRST) {
        st->first_seen++;
    }
    if (chunk->len > 0 && st->bytes + chunk->len <= sizeof(st->buf)) {
        memcpy(st->buf + st->bytes, chunk->data, chunk->len);
        st->bytes += chunk->len;
    }
    if (chunk->flags & STREAM_LAST) {
        st->last_seen++;
        reply_bytes(call, 0, st->buf, st->bytes);
    }
}

static void on_bidi(call_t *call, const request_t *req, const chunk_t *chunk,
                    void *user) {
    (void)req;
    (void)user;
    (void)reply_stream(call, 0, chunk);
}

typedef struct client_state {
    ntk_sem *sem;
    int status;
    char buf[256];
    size_t len;
} client_state_t;

static void on_response(int status, const response_t *resp, void *user) {
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    if (status == OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
        st->buf[resp->payload.len] = '\0';
    }
    ntk_sem_post(st->sem, 1);
}

static const char k_download[] = "0123456789ABCDEF";

static void on_download(call_t *call, const request_t *req, void *user) {
    size_t i;
    (void)req;
    (void)user;
    for (i = 0; i < 4; i++) {
        chunk_t chunk;
        memset(&chunk, 0, sizeof(chunk));
        chunk.data = k_download + i * 4;
        chunk.len = 4;
        chunk.offset = i * 4;
        chunk.flags = (i == 0 ? STREAM_FIRST : 0) | (i == 3 ? STREAM_LAST : 0);
        (void)reply_stream(call, 0, &chunk);
    }
}

typedef struct download_client {
    ntk_sem *sem;
    char buf[64];
    size_t len;
    int chunks;
    int last;
    int done;
    int status;
} download_client_t;

static void on_download_chunk(int status, const chunk_t *chunk, void *user) {
    download_client_t *c = (download_client_t *)user;
    if (status != OK) {
        return;
    }
    c->chunks++;
    if (chunk->flags & STREAM_LAST) {
        c->last++;
    }
    if (chunk->len > 0 && c->len + chunk->len <= sizeof(c->buf)) {
        memcpy(c->buf + c->len, chunk->data, chunk->len);
        c->len += chunk->len;
    }
}

static void on_download_done(int status, void *user) {
    download_client_t *c = (download_client_t *)user;
    c->done = 1;
    c->status = status;
    if (c->sem) {
        ntk_sem_post(c->sem, 1);
    }
}

static void on_bidi_chunk(int status, const chunk_t *chunk, void *user) {
    download_client_t *c = (download_client_t *)user;
    if (status != OK) {
        return;
    }
    c->chunks++;
    if (chunk->flags & STREAM_LAST) {
        c->last++;
    }
    if (c->len + chunk->len <= sizeof(c->buf)) {
        memcpy(c->buf + c->len, chunk->data, chunk->len);
        c->len += chunk->len;
    }
}

static int run_case(transport_kind_t transport, const char *label, uint16_t sdisc,
                    uint16_t cdisc) {
    node_config_t scfg;
    node_config_t ccfg;
    node_t *server = NULL;
    node_t *client = NULL;
    service_t *svc = NULL;
    proxy_t *proxy = NULL;
    stream_t *stream = NULL;
    upload_state_t upload;
    client_state_t cst;
    const char *expect = "hello world!";
    int failed = 0;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "stream-srv";
    scfg.transport = transport;
    scfg.bind_host = "127.0.0.1";
    scfg.discovery_host = "127.0.0.1";
    scfg.discovery_port = sdisc;
    scfg.io_threads = 1;
    scfg.hello_interval_ms = 200;
    scfg.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "stream-cli";
    ccfg.transport = transport;
    ccfg.bind_host = "127.0.0.1";
    ccfg.discovery_host = "127.0.0.1";
    ccfg.discovery_port = cdisc;
    ccfg.io_threads = 1;
    ccfg.hello_interval_ms = 200;
    ccfg.node_ttl_ms = 3000;

    if (node_create(&scfg, &server) != OK || node_create(&ccfg, &client) != OK) {
        rpc_log_error("[%s] node create failed", label);
        return 1;
    }
    (void)node_add_discovery_peer(server, "127.0.0.1", cdisc);
    (void)node_add_discovery_peer(client, "127.0.0.1", sdisc);

    memset(&upload, 0, sizeof(upload));
    if (service_create(server, "upload", &svc) != OK ||
        service_add_stream_method(svc, "chunk", on_upload, &upload) != OK ||
        service_add_stream_method(svc, "chat", on_bidi, NULL) != OK ||
        service_add_method(svc, "download", on_download, NULL) != OK) {
        rpc_log_error("[%s] service setup failed", label);
        node_destroy(client);
        node_destroy(server);
        return 1;
    }
    if (proxy_create(client, NULL, &proxy) != OK) {
        rpc_log_error("[%s] proxy create failed", label);
        node_destroy(client);
        node_destroy(server);
        return 1;
    }

    (void)node_wait_for_service(client, "upload", 5000);

    memset(&cst, 0, sizeof(cst));
    cst.sem = ntk_sem_create(0);
    if (!cst.sem) {
        failed = 1;
    } else if (proxy_stream_open(proxy, "upload", "chunk", on_response, &cst, 3000, &stream) !=
               OK) {
        rpc_log_error("[%s] open failed", label);
        failed = 1;
    } else {
        chunk_t chunk;
        memset(&chunk, 0, sizeof(chunk));
        chunk.data = "hello ";
        chunk.len = 6;
        chunk.offset = 0;
        chunk.flags = STREAM_FIRST;
        if (proxy_stream_send(stream, &chunk) != OK) {
            failed = 1;
        }
        chunk.data = "world";
        chunk.len = 5;
        chunk.offset = 6;
        chunk.flags = 0;
        if (proxy_stream_send(stream, &chunk) != OK) {
            failed = 1;
        }
        chunk.data = "!";
        chunk.len = 1;
        chunk.offset = 11;
        chunk.flags = STREAM_LAST;
        if (proxy_stream_send(stream, &chunk) != OK) {
            failed = 1;
        }
        if (ntk_sem_timedwait(cst.sem, 3000) != NTK_OK) {
            rpc_log_error("[%s] response timeout", label);
            failed = 1;
        } else {
            if (cst.status != OK || strcmp(cst.buf, expect) != 0) {
                rpc_log_error("[%s] bad response status=%d buf=%s", label, cst.status,
                              cst.buf);
                failed = 1;
            }
            if (upload.chunks != 3 || upload.first_seen != 1 || upload.last_seen != 1 ||
                upload.bytes != strlen(expect)) {
                rpc_log_error("[%s] server state chunks=%d first=%d last=%d bytes=%zu", label,
                              upload.chunks, upload.first_seen, upload.last_seen, upload.bytes);
                failed = 1;
            }
        }
    }
    if (stream) {
        (void)proxy_stream_destroy(stream);
    }
    if (cst.sem) {
        ntk_sem_destroy(cst.sem);
    }

    /* 下载方向：服务端流式响应，客户端按 chunk 接收。 */
    {
        download_client_t dc;
        uint64_t req_id = 0;
        memset(&dc, 0, sizeof(dc));
        dc.sem = ntk_sem_create(0);
        if (!dc.sem) {
            failed = 1;
        } else if (proxy_call_stream(proxy, "upload", "download", NULL, on_download_chunk,
                                          on_download_done, &dc, 3000, &req_id) != OK) {
            rpc_log_error("[%s] call_stream failed", label);
            failed = 1;
        } else if (ntk_sem_timedwait(dc.sem, 3000) != NTK_OK) {
            rpc_log_error("[%s] download timeout", label);
            failed = 1;
        } else if (dc.status != OK || dc.len != strlen(k_download) ||
                   memcmp(dc.buf, k_download, dc.len) != 0 || dc.chunks != 4 || dc.last != 1) {
            rpc_log_error("[%s] download mismatch status=%d len=%zu chunks=%d last=%d", label,
                          dc.status, dc.len, dc.chunks, dc.last);
            failed = 1;
        }
        if (dc.sem) {
            ntk_sem_destroy(dc.sem);
        }
    }

    /* 取消方向：发送 FIRST 后取消，本地回调应为 CLOSED，服务端清理流状态。 */
    {
        client_state_t cc;
        stream_t *s2 = NULL;
        memset(&cc, 0, sizeof(cc));
        cc.sem = ntk_sem_create(0);
        if (!cc.sem) {
            failed = 1;
        } else if (proxy_stream_open(proxy, "upload", "chunk", on_response, &cc, 3000, &s2) !=
                   OK) {
            rpc_log_error("[%s] cancel open failed", label);
            failed = 1;
        } else {
            chunk_t ck;
            memset(&ck, 0, sizeof(ck));
            ck.data = "x";
            ck.len = 1;
            ck.offset = 0;
            ck.flags = STREAM_FIRST;
            (void)proxy_stream_send(s2, &ck);
            (void)proxy_stream_cancel(s2);
            if (ntk_sem_timedwait(cc.sem, 3000) != NTK_OK) {
                rpc_log_error("[%s] cancel timeout", label);
                failed = 1;
            } else if (cc.status != ERR_CLOSED) {
                rpc_log_error("[%s] cancel status=%d", label, cc.status);
                failed = 1;
            }
            (void)proxy_stream_destroy(s2);
        }
        if (cc.sem) {
            ntk_sem_destroy(cc.sem);
        }
    }

    /* Full duplex：请求和响应同时按 chunk 进行。 */
    {
        download_client_t bc;
        stream_t *bidi = NULL;
        memset(&bc, 0, sizeof(bc));
        bc.sem = ntk_sem_create(0);
        if (!bc.sem || proxy_stream_open_bidi(proxy, "upload", "chat", on_bidi_chunk,
                                                   on_download_done, &bc, 3000, &bidi) != OK) {
            rpc_log_error("[%s] bidi open failed", label);
            failed = 1;
        } else {
            chunk_t ck;
            memset(&ck, 0, sizeof(ck));
            ck.data = "ping";
            ck.len = 4;
            ck.offset = 0;
            ck.flags = STREAM_FIRST;
            (void)proxy_stream_send(bidi, &ck);
            ck.data = "pong";
            ck.len = 4;
            ck.offset = 4;
            ck.flags = STREAM_LAST;
            (void)proxy_stream_send(bidi, &ck);
            if (ntk_sem_timedwait(bc.sem, 3000) != NTK_OK || bc.status != OK ||
                bc.len != 8 || memcmp(bc.buf, "pingpong", 8) != 0 || bc.chunks != 2 ||
                bc.last != 1) {
                rpc_log_error("[%s] bidi mismatch status=%d len=%zu chunks=%d last=%d", label,
                              bc.status, bc.len, bc.chunks, bc.last);
                failed = 1;
            }
            (void)proxy_stream_destroy(bidi);
        }
        if (bc.sem) {
            ntk_sem_destroy(bc.sem);
        }
    }

    proxy_destroy(proxy);
    node_destroy(client);
    node_destroy(server);

    if (failed) {
        rpc_log_error("[%s] FAIL", label);
    }
    return failed;
}

int main(void) {
    int failed = 0;
    failed += run_case(TRANSPORT_UDP, "udp", 43111, 43112);
    failed += run_case(TRANSPORT_TCP, "tcp", 43113, 43114);
    if (failed) {
        rpc_log_error("stream: FAIL");
        return 1;
    }
    rpc_log_info("stream: OK");
    return 0;
}
