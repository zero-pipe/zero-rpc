#ifndef ZTK_NET_UDP_SERVER_H
#define ZTK_NET_UDP_SERVER_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_poller_pool;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_poller_pool ztk_poller_pool;
typedef struct ztk_udp_server ztk_udp_server;

typedef struct ztk_udp_server_ops {
  void (*on_packet)(ztk_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                    const void *data, size_t len, void *user);
} ztk_udp_server_ops_t;

typedef struct ztk_udp_server_opts {
    const char *host;
    uint16_t port;
    /** SO_REUSEADDR；多 poller 时强制为 1 */
    int reuse;
    /**
     * SO_REUSEPORT：每个 poller 独立 bind 同端口，内核将入站包分到各 fd（抢占收包）。
     * poller_count > 1 时自动开启；单 poller 时可显式置 1。
     */
    int reuse_port;

    ztk_poller *poller;
    /** 与 poller 二选一 */
    ztk_poller_pool *poller_pool;

    const ztk_udp_server_ops_t *ops;
    void *user;
} ztk_udp_server_opts_t;

ZTK_API ztk_udp_server *ztk_udp_server_create(const ztk_udp_server_opts_t *opts);
ZTK_API void ztk_udp_server_destroy(ztk_udp_server *srv);

ZTK_API ztk_err_t ztk_udp_server_start(ztk_udp_server *srv);
ZTK_API void ztk_udp_server_stop(ztk_udp_server *srv);

ZTK_API uint16_t ztk_udp_server_port(const ztk_udp_server *srv);
ZTK_API unsigned ztk_udp_server_poller_count(const ztk_udp_server *srv);
ZTK_API ztk_poller *ztk_udp_server_poller(const ztk_udp_server *srv, unsigned index);

ZTK_API ztk_err_t ztk_udp_server_sendto(ztk_udp_server *srv, const char *ip, uint16_t port,
                                           const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_UDP_SERVER_H */
