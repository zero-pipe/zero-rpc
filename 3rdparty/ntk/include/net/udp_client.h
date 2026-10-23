#ifndef NTK_NET_UDP_CLIENT_H
#define NTK_NET_UDP_CLIENT_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_udp_client ntk_udp_client;

typedef struct ntk_udp_client_ops {
    void (*on_packet)(ntk_udp_client *client, const void *data, size_t len, const char *from_ip,
                      uint16_t from_port, void *user);
    void (*on_error)(ntk_udp_client *client, void *user);
} ntk_udp_client_ops_t;

typedef struct ntk_udp_client_opts {
    ntk_poller *poller;
    const ntk_udp_client_ops_t *ops;
    void *user;
} ntk_udp_client_opts_t;

NTK_API ntk_udp_client *ntk_udp_client_create(const ntk_udp_client_opts_t *opts);
NTK_API void ntk_udp_client_destroy(ntk_udp_client *client);

/**
 * 绑定本地 UDP，并记录默认对端（用于 ntk_udp_client_send）。
 * local_port=0 由系统分配；peer 可为组播地址。
 */
NTK_API ntk_err_t ntk_udp_client_start(ntk_udp_client *client, const char *peer_host, uint16_t peer_port,
                                          const char *local_host, uint16_t local_port, int reuse);

/** 仅绑定本地端口并挂到 poller；推流服务端收 RTP 前使用，对端由 set_peer 或 sendto 指定 */
NTK_API ntk_err_t ntk_udp_client_bind(ntk_udp_client *client, const char *local_host, uint16_t local_port,
                                         int reuse);

/** NAT 打洞后更新默认 send 目标 */
NTK_API void ntk_udp_client_set_peer(ntk_udp_client *client, const char *peer_host, uint16_t peer_port);

NTK_API ntk_err_t ntk_udp_client_send(ntk_udp_client *client, const void *data, size_t len);
NTK_API ntk_err_t ntk_udp_client_sendto(ntk_udp_client *client, const void *data, size_t len,
                                           const char *ip, uint16_t port);

NTK_API uint16_t ntk_udp_client_local_port(const ntk_udp_client *client);

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_UDP_CLIENT_H */
