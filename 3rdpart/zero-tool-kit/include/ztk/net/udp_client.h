#ifndef ZTK_NET_UDP_CLIENT_H
#define ZTK_NET_UDP_CLIENT_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_udp_client ztk_udp_client;

typedef struct ztk_udp_client_ops {
    void (*on_packet)(ztk_udp_client *client, const void *data, size_t len, const char *from_ip,
                      uint16_t from_port, void *user);
    void (*on_error)(ztk_udp_client *client, void *user);
} ztk_udp_client_ops_t;

typedef struct ztk_udp_client_opts {
    ztk_poller *poller;
    const ztk_udp_client_ops_t *ops;
    void *user;
} ztk_udp_client_opts_t;

ZTK_API ztk_udp_client *ztk_udp_client_create(const ztk_udp_client_opts_t *opts);
ZTK_API void ztk_udp_client_destroy(ztk_udp_client *client);

/**
 * 绑定本地 UDP，并记录默认对端（用于 ztk_udp_client_send）。
 * local_port=0 由系统分配；peer 可为组播地址。
 */
ZTK_API ztk_err_t ztk_udp_client_start(ztk_udp_client *client, const char *peer_host, uint16_t peer_port,
                                          const char *local_host, uint16_t local_port, int reuse);

/** 仅绑定本地端口并挂到 poller；推流服务端收 RTP 前使用，对端由 set_peer 或 sendto 指定 */
ZTK_API ztk_err_t ztk_udp_client_bind(ztk_udp_client *client, const char *local_host, uint16_t local_port,
                                         int reuse);

/** NAT 打洞后更新默认 send 目标 */
ZTK_API void ztk_udp_client_set_peer(ztk_udp_client *client, const char *peer_host, uint16_t peer_port);

ZTK_API ztk_err_t ztk_udp_client_send(ztk_udp_client *client, const void *data, size_t len);
ZTK_API ztk_err_t ztk_udp_client_sendto(ztk_udp_client *client, const void *data, size_t len,
                                           const char *ip, uint16_t port);

ZTK_API uint16_t ztk_udp_client_local_port(const ztk_udp_client *client);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_UDP_CLIENT_H */
