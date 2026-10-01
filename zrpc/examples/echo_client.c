/*
 * 示例：客户端。演示：
 *   - 用 config 创建节点；
 *   - 注册一个自定义 codec（payload 序列化扩展点）；
 *   - 异步 call + 回调里解码 payload。
 *
 * 用法：
 *   zrpc_echo_client [config.json]
 */
#include <zrpc/zrpc.h>
#include <ztk/thread/sem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 一个最小的 "text" codec：业务对象 <-> 字节流 ---- */
static int text_encode(void *ctx, const void *obj, void **out_data, size_t *out_len) {
    const char *s = (const char *)obj;
    size_t n = strlen(s);
    char *b = (char *)malloc(n ? n : 1);
    (void)ctx;
    if (!b) {
        return ZRPC_ERR_NOMEM;
    }
    memcpy(b, s, n);
    *out_data = b;
    *out_len = n;
    return ZRPC_OK;
}

static int text_decode(void *ctx, const void *data, size_t len, void **out_obj) {
    char *b = (char *)malloc(len + 1);
    (void)ctx;
    if (!b) {
        return ZRPC_ERR_NOMEM;
    }
    memcpy(b, data, len);
    b[len] = '\0';
    *out_obj = b;
    return ZRPC_OK;
}

static void text_release(void *ctx, void *obj) {
    (void)ctx;
    free(obj);
}

static const zrpc_codec_t g_text_codec = {
    "text", 1, text_encode, text_decode, text_release, NULL,
};

static ztk_sem *g_done;

static void on_response(int status, const zrpc_response_t *resp, void *user) {
    zrpc_node_t *node = (zrpc_node_t *)user;
    if (status == ZRPC_OK && resp) {
        const zrpc_codec_t *codec = zrpc_node_find_codec(node, resp->payload.encoding);
        if (codec && codec->decode) {
            void *obj = NULL;
            if (codec->decode(codec->ctx, resp->payload.data, resp->payload.len, &obj) == ZRPC_OK) {
                printf("response: %s\n", (const char *)obj);
                codec->release(codec->ctx, obj);
            }
        } else {
            printf("response: %.*s\n", (int)resp->payload.len, (const char *)resp->payload.data);
        }
    } else {
        printf("call failed: status=%d\n", status);
    }
    ztk_sem_post(g_done, 1);
}

int main(int argc, char **argv) {
    zrpc_config_t cfg;
    zrpc_node_t *node = NULL;
    zrpc_proxy_t *proxy = NULL;
    zrpc_payload_t payload;
    void *bytes = NULL;
    size_t blen = 0;
    int rc;

    if (argc > 1) {
        rc = zrpc_config_load_file(argv[1], &cfg);
        if (rc != ZRPC_OK) {
            fprintf(stderr, "load config failed: %d\n", rc);
            return 1;
        }
    } else {
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.name, sizeof(cfg.name), "echo_client");
        snprintf(cfg.bind_host, sizeof(cfg.bind_host), "127.0.0.1");
        snprintf(cfg.discovery_host, sizeof(cfg.discovery_host), "127.0.0.1");
        cfg.node.name = cfg.name;
        cfg.node.bind_host = cfg.bind_host;
        cfg.node.discovery_host = cfg.discovery_host;
        cfg.node.transport = ZRPC_TRANSPORT_UDP;
        cfg.node.io_threads = 1;
        cfg.node.discovery_port = 42301;
        cfg.node.hello_interval_ms = 200;
        /* 显式发现服务端 */
        cfg.peer_count = 1;
        snprintf(cfg.peers[0].ip, sizeof(cfg.peers[0].ip), "127.0.0.1");
        cfg.peers[0].port = 42300;
    }

    rc = zrpc_node_create_from_config(&cfg, &node);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "node create failed: %d\n", rc);
        return 1;
    }
    (void)zrpc_node_register_codec(node, &g_text_codec);
    rc = zrpc_proxy_create(node, NULL, &proxy);
    if (rc != ZRPC_OK) {
        zrpc_node_destroy(node);
        return 1;
    }

    /* 用 codec 把业务对象编码成 payload；encoding=1 会随消息透传。 */
    g_text_codec.encode(NULL, "hello-from-client", &bytes, &blen);
    payload.data = bytes;
    payload.len = blen;
    payload.encoding = g_text_codec.encoding;

    g_done = ztk_sem_create(0);
    if (!g_done) {
        zrpc_node_destroy(node);
        return 1;
    }

    zrpc_node_wait_for_service(node, "echo", 5000);
    rc = zrpc_proxy_call(proxy, "echo", "ping", &payload, on_response, node, 2000, NULL);
    if (rc == ZRPC_OK) {
        (void)ztk_sem_timedwait(g_done, 3000);
    } else {
        printf("proxy_call failed: %d\n", rc);
    }

    g_text_codec.release(NULL, bytes);
    ztk_sem_destroy(g_done);
    zrpc_proxy_destroy(proxy);
    zrpc_node_destroy(node);
    return 0;
}
