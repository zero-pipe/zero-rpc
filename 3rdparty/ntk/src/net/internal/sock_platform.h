#ifndef NTK_SOCK_PLATFORM_H
#define NTK_SOCK_PLATFORM_H

#include "ntk_errno.h"
#include "net/socket.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ntk_err_t ntk_sockplat_init(void);
void ntk_sockplat_fini(void);

ntk_err_t ntk_sockplat_apply_tcp_server_opts(int fd);
ntk_err_t ntk_sockplat_apply_tcp_client_opts(int fd);
ntk_err_t ntk_sockplat_apply_udp_opts(int fd);

ntk_err_t ntk_sockplat_listen(int *out_fd, const char *host, uint16_t port, int backlog);
/** @param reuse_addr SO_REUSEADDR；@param reuse_port SO_REUSEPORT（多 socket 同端口抢占收包，Linux 等） */
ntk_err_t ntk_sockplat_bind_udp(int *out_fd, const char *host, uint16_t port, int reuse_addr,
                                  int reuse_port);
ntk_err_t ntk_sockplat_connect(int *out_fd, const char *host, uint16_t port, int *in_progress);
ntk_err_t ntk_sockplat_accept(int listen_fd, int *out_fd);

typedef struct ntk_sock_iov {
    const void *base;
    size_t len;
} ntk_sock_iov;

#define NTK_SOCK_IOV_MAX 16

ntk_ssize_t ntk_sockplat_send(int fd, const void *buf, size_t len);
/** 聚合写；返回已发送总字节或负错误码 */
ntk_ssize_t ntk_sockplat_sendv(int fd, const ntk_sock_iov *iov, unsigned count);
ntk_ssize_t ntk_sockplat_recv(int fd, void *buf, size_t len);
ntk_ssize_t ntk_sockplat_sendto(int fd, const void *buf, size_t len, const char *ip, uint16_t port);
ntk_ssize_t ntk_sockplat_recvfrom(int fd, void *buf, size_t len, char *ip, size_t ip_len, uint16_t *port);

/** 批量 UDP 收发（Linux: sendmmsg/recvmmsg；其他平台循环单发/单收）。 */
int ntk_sockplat_sendmmsg(int fd, const ntk_udp_dgram_t *msgs, unsigned count);
int ntk_sockplat_recvmmsg(int fd, ntk_udp_recv_t *msgs, unsigned cap);

ntk_err_t ntk_sockplat_get_local(int fd, char *ip, size_t ip_len, uint16_t *port);
ntk_err_t ntk_sockplat_get_peer(int fd, char *ip, size_t ip_len, uint16_t *port);
ntk_err_t ntk_sockplat_check_connect(int fd);
ntk_err_t ntk_sockplat_close(int fd);

/** Linux: TCP_CONGESTION=bbr；其他平台可能返回 NTK_ERR_NOT_IMPL */
ntk_err_t ntk_sockplat_set_tcp_bbr(int fd, int on);

/** UDP 广播（SO_BROADCAST） */
ntk_err_t ntk_sockplat_set_broadcast(int fd, int on);

/** 组播 TTL / 出口网卡 / 是否回环本机组播包 */
ntk_err_t ntk_sockplat_set_multicast_ttl(int fd, uint8_t ttl);
ntk_err_t ntk_sockplat_set_multicast_if(int fd, const char *local_ip);
ntk_err_t ntk_sockplat_set_multicast_loop(int fd, int on);

/** 加入/离开 IPv4 组播组；local_if 可为 NULL / 0.0.0.0 */
ntk_err_t ntk_sockplat_join_multicast(int fd, const char *group_addr, const char *local_if);
ntk_err_t ntk_sockplat_leave_multicast(int fd, const char *group_addr, const char *local_if);

/** 源特定组播（SSM）；src_ip 为发送方单播地址 */
ntk_err_t ntk_sockplat_join_multicast_filter(int fd, const char *group_addr, const char *src_ip,
                                               const char *local_if);
ntk_err_t ntk_sockplat_leave_multicast_filter(int fd, const char *group_addr, const char *src_ip,
                                                const char *local_if);

#ifdef __cplusplus
}
#endif

#endif /* NTK_SOCK_PLATFORM_H */
