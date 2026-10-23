#ifndef NTK_NET_UDP_SERVER_H
#define NTK_NET_UDP_SERVER_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
struct ntk_poller_pool;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_poller_pool ntk_poller_pool;
typedef struct ntk_udp_server ntk_udp_server;

typedef struct ntk_udp_server_ops {
  void (*on_packet)(ntk_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                    const void *data, size_t len, void *user);
} ntk_udp_server_ops_t;

typedef struct ntk_udp_server_opts {
    const char *host;
    uint16_t port;
    /** SO_REUSEADDR；多 poller 时强制为 1 */
    int reuse;
    /**
     * SO_REUSEPORT：每个 poller 独立 bind 同端口，内核将入站包分到各 fd（抢占收包）。
     * poller_count > 1 时自动开启；单 poller 时可显式置 1。
     */
    int reuse_port;

    ntk_poller *poller;
    /** 与 poller 二选一 */
    ntk_poller_pool *poller_pool;

    const ntk_udp_server_ops_t *ops;
    void *user;
} ntk_udp_server_opts_t;

NTK_API ntk_udp_server *ntk_udp_server_create(const ntk_udp_server_opts_t *opts);
NTK_API void ntk_udp_server_destroy(ntk_udp_server *srv);

NTK_API ntk_err_t ntk_udp_server_start(ntk_udp_server *srv);
NTK_API void ntk_udp_server_stop(ntk_udp_server *srv);

NTK_API uint16_t ntk_udp_server_port(const ntk_udp_server *srv);
NTK_API unsigned ntk_udp_server_poller_count(const ntk_udp_server *srv);
NTK_API ntk_poller *ntk_udp_server_poller(const ntk_udp_server *srv, unsigned index);

NTK_API ntk_err_t ntk_udp_server_sendto(ntk_udp_server *srv, const char *ip, uint16_t port,
                                           const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_UDP_SERVER_H */
