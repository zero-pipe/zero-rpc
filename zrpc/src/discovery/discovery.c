/* mesh 服务发现：UDP 广播 HELLO/BYE + 对端注册表。
 *
 * 只通过 zri_node_access.h 的门面访问 node 状态，不穿透 struct zrpc_node。 */
#include "discovery.h"

#include <zrpc/zrpc_node.h>

#include "node_access.h"
#include "registry_backend.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define ZRPC_DISC_MAGIC 0x5a525043u /* 'ZRPC' */
#define ZRPC_DISC_VERSION 3
#define ZRPC_DISC_FLAG_HELLO 0x01
#define ZRPC_DISC_FLAG_BYE 0x02
#define ZRPC_DISC_FRAME_MAX 1400

/* 构建 HELLO/BYE 帧。返回长度，0 失败。 */
static size_t disc_build(zrpc_node_t *node, int bye, uint8_t *buf, size_t cap) {
    size_t n = 0;
    size_t svc_count = zri_node_service_count(node);
    const char *name = zrpc_node_name(node);
    size_t name_len = name ? strlen(name) : 0;
    size_t binding_count = zrpc_node_binding_count(node);
    size_t i;

    if (name_len > 255) {
        name_len = 255;
    }
    if (svc_count > 0xffffu) {
        svc_count = 0xffffu;
    }
    if (binding_count > 255u) {
        binding_count = 255u;
    }
    if (cap < 12 + name_len + 1 + 2) {
        return 0;
    }
    zri_put_u32(buf + n, ZRPC_DISC_MAGIC);
    n += 4;
    buf[n++] = ZRPC_DISC_VERSION;
    buf[n++] = (uint8_t)(bye ? ZRPC_DISC_FLAG_BYE : ZRPC_DISC_FLAG_HELLO);
    zri_put_u32(buf + n, zri_node_id(node));
    n += 4;
    buf[n++] = (uint8_t)name_len;
    if (name_len) {
        memcpy(buf + n, name, name_len);
        n += name_len;
    }
    buf[n++] = (uint8_t)binding_count;
    for (i = 0; i < binding_count; i++) {
        const char *scheme = zri_node_binding_scheme(node, i);
        size_t sl = scheme ? strlen(scheme) : 0;
        if (!scheme || sl == 0 || sl > 255 || n + 1 + sl + 2 > cap) {
            return 0;
        }
        buf[n++] = (uint8_t)sl;
        memcpy(buf + n, scheme, sl);
        n += sl;
        zri_put_u16(buf + n, zri_node_binding_port(node, i));
        n += 2;
    }
    zri_put_u16(buf + n, (uint16_t)svc_count);
    n += 2;

    for (i = 0; i < svc_count; i++) {
        const char *svc_name = zri_node_service_name(node, i);
        size_t sl = svc_name ? strlen(svc_name) : 0;
        if (sl > 255) {
            sl = 255;
        }
        if (n + 1 + sl > cap) {
            break;
        }
        buf[n++] = (uint8_t)sl;
        memcpy(buf + n, svc_name, sl);
        n += sl;
    }
    return n;
}

static void disc_send_to(zrpc_node_t *node, const char *ip, uint16_t port, const uint8_t *buf,
                         size_t len) {
    zri_discovery_state_t *st = zri_node_discovery(node);
    if (st && st->sock && ip && port) {
        (void)ztk_socket_sendto(st->sock, buf, len, ip, port);
    }
}

void zri_discovery_send_hello(zrpc_node_t *node, int bye) {
    uint8_t buf[ZRPC_DISC_FRAME_MAX];
    zri_discovery_state_t *st;
    const char *host;
    size_t n;
    int i;

    if (!node) {
        return;
    }
    st = zri_node_discovery(node);
    if (!st || !st->sock) {
        return;
    }
    n = disc_build(node, bye, buf, sizeof(buf));
    if (n == 0) {
        return;
    }
    host = zri_node_discovery_host(node);
    disc_send_to(node, host && host[0] ? host : "255.255.255.255",
                 zrpc_node_discovery_port(node), buf, n);
    for (i = 0; i < ZRPC_DISC_TARGET_MAX; i++) {
        if (st->targets[i].used) {
            disc_send_to(node, st->targets[i].ip, st->targets[i].port, buf, n);
        }
    }
}

static void peer_store(zrpc_node_t *node, const char *ip, uint32_t node_id,
                       const char *remote_name, const uint8_t *binding_blob, size_t binding_len,
                       const uint8_t *svc_blob, int svc_count, size_t svc_len, uint64_t now_ms) {
    zri_discovery_state_t *st = zri_node_discovery(node);
    zri_peer_t *p = NULL;
    int i;
    size_t off = 0;
    size_t binding_off = 0;

    if (!st) {
        return;
    }
    for (i = 0; i < ZRPC_MAX_PEERS; i++) {
        if (st->peers[i].used && st->peers[i].node_id == node_id) {
            p = &st->peers[i];
            break;
        }
    }
    if (!p) {
        for (i = 0; i < ZRPC_MAX_PEERS; i++) {
            if (!st->peers[i].used) {
                p = &st->peers[i];
                memset(p, 0, sizeof(*p));
                p->used = 1;
                break;
            }
        }
    }
    if (!p) {
        return;
    }
    p->node_id = node_id;
    p->last_seen_ms = now_ms;
    snprintf(p->ip_s, sizeof(p->ip_s), "%s", ip);

    p->endpoint_count = 0;
    while (binding_off < binding_len && p->endpoint_count < ZRPC_MAX_BINDINGS) {
        uint8_t sl = binding_blob[binding_off++];
        zrpc_endpoint_t *endpoint;
        if (sl == 0 || binding_off + sl + 2 > binding_len) {
            break;
        }
        endpoint = &p->endpoints[p->endpoint_count];
        if (sl >= sizeof(endpoint->scheme)) {
            break;
        }
        memcpy(endpoint->scheme, binding_blob + binding_off, sl);
        endpoint->scheme[sl] = '\0';
        binding_off += sl;
        endpoint->port = zri_get_u16(binding_blob + binding_off);
        binding_off += 2;
        endpoint->transport = zrpc_transport_kind_from_scheme(endpoint->scheme);
        zri_copy_str(endpoint->host, sizeof(endpoint->host), ip, "0.0.0.0");
        if (endpoint->port && (int)endpoint->transport >= 0) {
            p->endpoint_count++;
        }
    }

    p->service_count = 0;
    while (off < svc_len && p->service_count < ZRPC_PEER_MAX_SERVICES) {
        uint8_t sl = svc_blob[off];
        off++;
        if (off + sl > svc_len) {
            break;
        }
        if (sl > 0 && sl < ZRPC_SERVICE_NAME_MAX) {
            memcpy(p->services[p->service_count], svc_blob + off, sl);
            p->services[p->service_count][sl] = '\0';
            p->service_count++;
        }
        off += sl;
    }
    {
        char instance_id[ZRPC_NAME_MAX];
        int n = snprintf(instance_id, sizeof(instance_id), "%s-%08x",
                         remote_name && remote_name[0] ? remote_name : "node",
                         (unsigned)node_id);
        if (n > 0 && (size_t)n < sizeof(instance_id) && p->endpoint_count > 0) {
            const char *services[ZRPC_PEER_MAX_SERVICES];
            for (i = 0; i < p->service_count; i++) {
                services[i] = p->services[i];
            }
            (void)zri_registry_replace_node(zri_node_registry(node), node_id, instance_id, services,
                                             (size_t)p->service_count, p->endpoints,
                                             p->endpoint_count, now_ms);
        }
    }
    (void)svc_count;
}

static void disc_handle_frame(zrpc_node_t *node, const char *ip, const uint8_t *buf, size_t len) {
    zri_discovery_state_t *st = zri_node_discovery(node);
    uint32_t magic;
    uint32_t node_id;
    uint8_t flags;
    size_t off;
    uint8_t name_len;
    char remote_name[ZRPC_NAME_MAX];
    uint16_t svc_count;
    size_t svc_len;
    size_t binding_len;
    size_t binding_start;
    uint64_t now = zri_now_ms();
    int i;

    if (!st || len < 13) {
        return;
    }
    magic = zri_get_u32(buf);
    if (magic != ZRPC_DISC_MAGIC || buf[4] != ZRPC_DISC_VERSION) {
        return;
    }
    flags = buf[5];
    node_id = zri_get_u32(buf + 6);
    if (node_id == zri_node_id(node)) {
        return; /* 自己 */
    }
    off = 10;
    name_len = buf[off++];
    if (off + name_len + 2 > len) {
        return;
    }
    if (name_len >= sizeof(remote_name)) {
        return;
    }
    memcpy(remote_name, buf + off, name_len);
    remote_name[name_len] = '\0';
    off += name_len;
    if (off >= len) {
        return;
    }
    {
        uint8_t binding_count = buf[off++];
        binding_start = off;
        for (i = 0; i < binding_count; i++) {
            uint8_t sl;
            if (off >= len) {
                return;
            }
            sl = buf[off++];
            if (off + sl + 2 > len) {
                return;
            }
            off += sl + 2;
        }
        binding_len = off - binding_start;
    }
    if (off + 2 > len) {
        return;
    }
    svc_count = zri_get_u16(buf + off);
    off += 2;
    svc_len = len - off;

    if (flags & ZRPC_DISC_FLAG_BYE) {
        for (i = 0; i < ZRPC_MAX_PEERS; i++) {
            if (st->peers[i].used && st->peers[i].node_id == node_id) {
                st->peers[i].used = 0;
                break;
            }
        }
        zri_registry_remove_node(zri_node_registry(node), node_id);
        return;
    }
    if (flags & ZRPC_DISC_FLAG_HELLO) {
        peer_store(node, ip, node_id, remote_name, buf + binding_start, binding_len, buf + off,
                   svc_count, svc_len, now);
    }
}

static void disc_recv_cb(ztk_socket *sock, void *user) {
    zrpc_node_t *node = (zrpc_node_t *)user;
    zri_discovery_state_t *st = zri_node_discovery(node);
    uint8_t buf[ZRPC_DISC_FRAME_MAX];
    (void)sock;
    if (!st || !st->sock) {
        return;
    }
    for (;;) {
        char ip[ZRPC_ENDPOINT_MAX];
        uint16_t port = 0;
        ztk_ssize_t n = ztk_socket_recvfrom(st->sock, buf, sizeof(buf), ip, sizeof(ip), &port);
        if (n <= 0) {
            break;
        }
        disc_handle_frame(node, ip, buf, (size_t)n);
    }
}

int zri_discovery_init(zrpc_node_t *node) {
    zri_discovery_state_t *st = zri_node_discovery(node);
    ztk_socket_callbacks_t cbs;
    uint16_t actual = 0;
#if defined(_WIN32)
    const int reuse_port = 0;
#else
    const int reuse_port = 1;
#endif

    if (!st) {
        return ZRPC_ERR_INVALID;
    }
    st->sock = ztk_socket_create();
    if (!st->sock) {
        return ZRPC_ERR_NOMEM;
    }
    if (ztk_socket_bind_udp_ex(st->sock, "0.0.0.0", zrpc_node_discovery_port(node), 1, reuse_port) !=
        ZTK_OK) {
        return ZRPC_ERR_IO;
    }
    (void)ztk_socket_set_broadcast(st->sock, 1);
    if (ztk_socket_get_local(st->sock, NULL, 0, &actual) == ZTK_OK) {
        zri_node_set_discovery_port(node, actual);
    }
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_readable = disc_recv_cb;
    if (ztk_socket_attach_poller(st->sock, zri_node_poller(node), &cbs, node) != ZTK_OK) {
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

void zri_discovery_shutdown(zrpc_node_t *node) {
    zri_discovery_state_t *st = zri_node_discovery(node);
    if (st && st->sock) {
        ztk_socket_detach_poller(st->sock);
        ztk_socket_destroy(st->sock);
        st->sock = NULL;
    }
}

void zri_discovery_on_readable(zrpc_node_t *node) {
    zri_discovery_state_t *st = zri_node_discovery(node);
    if (st && st->sock) {
        disc_recv_cb(st->sock, node);
    }
}

void zri_discovery_tick(zrpc_node_t *node, uint64_t now_ms) {
    zri_discovery_state_t *st = zri_node_discovery(node);
    uint32_t ttl;
    int i;
    if (!st) {
        return;
    }
    ttl = zri_node_ttl_ms(node);
    for (i = 0; i < ZRPC_MAX_PEERS; i++) {
        zri_peer_t *p = &st->peers[i];
        if (p->used && now_ms - p->last_seen_ms > ttl) {
            p->used = 0;
        }
    }
    zri_registry_expire(zri_node_registry(node), now_ms, ttl);
}

int zri_discovery_find(zrpc_node_t *node, const char *service, char *ip_out, size_t ip_cap,
                        uint16_t *port_out, zrpc_transport_kind_t *transport_out) {
    zrpc_endpoint_t endpoint;
    if (!node || !service || !ip_out || !port_out) {
        return ZRPC_ERR_INVALID;
    }
    if (zri_registry_select(zri_node_registry(node), service, &endpoint) == ZRPC_OK) {
        zri_copy_str(ip_out, ip_cap, endpoint.host, "0.0.0.0");
        *port_out = endpoint.port;
        if (transport_out) {
            *transport_out = endpoint.transport;
        }
        return ZRPC_OK;
    }
    return ZRPC_ERR_NOTFOUND;
}

static int mesh_backend_start(zrpc_node_t *node);
static void mesh_backend_stop(zrpc_node_t *node);
static void mesh_backend_tick(zrpc_node_t *node, uint64_t now_ms);
static void mesh_backend_announce(zrpc_node_t *node, int bye);

static const zri_registry_backend_ops_t g_mesh_ops = {
    mesh_backend_start,
    mesh_backend_stop,
    mesh_backend_tick,
    mesh_backend_announce,
};

static const zri_registry_backend_t g_mesh_backend = {
    "mesh",
    &g_mesh_ops,
};

const zri_registry_backend_t *zri_registry_backend_mesh(void) {
    return &g_mesh_backend;
}

static int mesh_backend_start(zrpc_node_t *node) {
    return zri_discovery_init(node);
}

static void mesh_backend_stop(zrpc_node_t *node) {
    zri_discovery_shutdown(node);
}

static void mesh_backend_tick(zrpc_node_t *node, uint64_t now_ms) {
    zri_discovery_tick(node, now_ms);
}

static void mesh_backend_announce(zrpc_node_t *node, int bye) {
    zri_discovery_send_hello(node, bye);
}
