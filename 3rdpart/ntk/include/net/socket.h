#ifndef NTK_NET_SOCKET_H
#define NTK_NET_SOCKET_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#  include <BaseTsd.h>
typedef SSIZE_T ntk_ssize_t;
#elif defined(__GNUC__) || defined(__linux__)
#  include <sys/types.h>
typedef ssize_t ntk_ssize_t;
#else
typedef ptrdiff_t ntk_ssize_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_socket ntk_socket;

typedef enum ntk_sock_type {
    NTK_SOCK_NONE = 0,
    NTK_SOCK_TCP_CLIENT,
    NTK_SOCK_TCP_LISTEN,
    NTK_SOCK_TCP_CONNECTED,
    NTK_SOCK_UDP
} ntk_sock_type_t;

/**
 * Poller 就绪回调（在 poll 线程内调用，勿阻塞）。
 * on_readable：可读（TCP 已连接 / UDP / listen 可 accept）
 * on_writable：可写（含异步 connect 完成前需监听 OUT）
 * on_error：错误或对端关闭（EPOLLERR/HUP）
 */
typedef struct ntk_socket_callbacks {
    void (*on_readable)(ntk_socket *sock, void *user);
    void (*on_writable)(ntk_socket *sock, void *user);
    void (*on_error)(ntk_socket *sock, void *user);
} ntk_socket_callbacks_t;

NTK_API ntk_socket *ntk_socket_create(void);
NTK_API void ntk_socket_destroy(ntk_socket *sock);

/** 接管已有 fd（设为非阻塞 + CLOEXEC），成功后由 ntk_socket 负责 close */
NTK_API ntk_err_t ntk_socket_adopt(ntk_socket *sock, int fd, ntk_sock_type_t type);

NTK_API int ntk_socket_fd(const ntk_socket *sock);
NTK_API ntk_sock_type_t ntk_socket_type(const ntk_socket *sock);

/** host 可为 NULL / "" / "0.0.0.0"；port=0 表示由系统分配 */
NTK_API ntk_err_t ntk_socket_listen(ntk_socket *sock, const char *host, uint16_t port, int backlog);

/** @param reuse SO_REUSEADDR */
NTK_API ntk_err_t ntk_socket_bind_udp(ntk_socket *sock, const char *host, uint16_t port, int reuse);

/** reuse_addr / reuse_port 对应 SO_REUSEADDR / SO_REUSEPORT */
NTK_API ntk_err_t ntk_socket_bind_udp_ex(ntk_socket *sock, const char *host, uint16_t port,
                                            int reuse_addr, int reuse_port);

/**
 * TCP 连接。已连接返回 NTK_OK；非阻塞进行中也返回 NTK_OK 且 *in_progress=1。
 * 失败返回负错误码。
 */
NTK_API ntk_err_t ntk_socket_connect(ntk_socket *sock, const char *host, uint16_t port, int *in_progress);

/** 异步 connect 后，在 on_writable 或 poll OUT 后调用，检查是否真正连通 */
NTK_API ntk_err_t ntk_socket_check_connect(ntk_socket *sock);

/** 从监听 socket accept，*out_client 为新分配的 socket */
NTK_API ntk_err_t ntk_socket_accept(ntk_socket *listen_sock, ntk_socket **out_client);

typedef struct ntk_socket_iov {
    const void *base;
    size_t len;
} ntk_socket_iov;

#define NTK_SOCKET_IOV_MAX 16

NTK_API ntk_ssize_t ntk_socket_send(ntk_socket *sock, const void *data, size_t len);
NTK_API ntk_ssize_t ntk_socket_sendv(ntk_socket *sock, const ntk_socket_iov *iov, unsigned count);

/** Linux 可选 BBR；Win32 返回 NTK_ERR_NOT_IMPL */
NTK_API ntk_err_t ntk_socket_set_bbr(ntk_socket *sock, int on);
NTK_API ntk_ssize_t ntk_socket_recv(ntk_socket *sock, void *buf, size_t len);

NTK_API ntk_ssize_t ntk_socket_sendto(ntk_socket *sock, const void *data, size_t len,
                                         const char *ip, uint16_t port);
NTK_API ntk_ssize_t ntk_socket_recvfrom(ntk_socket *sock, void *buf, size_t len,
                                           char *ip, size_t ip_len, uint16_t *port);

/** UDP 批量发送元素。 */
typedef struct ntk_udp_dgram {
    const void *data;
    size_t len;
    const char *ip;
    uint16_t port;
} ntk_udp_dgram_t;

/** UDP 批量接收元素（data/cap 入参，len/ip/port 出参）。 */
typedef struct ntk_udp_recv {
    void *data;
    size_t cap;
    size_t len;
    char ip[46];
    uint16_t port;
} ntk_udp_recv_t;

/**
 * 批量发送多个 UDP 数据报（Linux: sendmmsg，一次系统调用）。
 * @return 实际发送条数（>=0），或负错误码。
 */
NTK_API int ntk_socket_sendto_batch(ntk_socket *sock, const ntk_udp_dgram_t *msgs, unsigned count);

/**
 * 批量接收多个 UDP 数据报（Linux: recvmmsg，一次系统调用）。
 * @param msgs 数组，每项 data/cap 由调用方提供
 * @return 实际接收条数（>=0，0 表示无更多数据），或负错误码。
 */
NTK_API int ntk_socket_recvfrom_batch(ntk_socket *sock, ntk_udp_recv_t *msgs, unsigned cap);

NTK_API ntk_err_t ntk_socket_get_local(const ntk_socket *sock, char *ip, size_t ip_len, uint16_t *port);
NTK_API ntk_err_t ntk_socket_get_peer(const ntk_socket *sock, char *ip, size_t ip_len, uint16_t *port);

/** 注册到 poller；listen 默认 IN，connect 进行中会附加 OUT */
NTK_API ntk_err_t ntk_socket_attach_poller(ntk_socket *sock, ntk_poller *poller,
                                              const ntk_socket_callbacks_t *cb, void *user);
NTK_API ntk_err_t ntk_socket_detach_poller(ntk_socket *sock);

/** 修改关注事件（已 attach 后） */
NTK_API ntk_err_t ntk_socket_mod_events(ntk_socket *sock, unsigned events);

/** UDP：开启广播发送（SO_BROADCAST） */
NTK_API ntk_err_t ntk_socket_set_broadcast(ntk_socket *sock, int on);

NTK_API ntk_err_t ntk_socket_set_multicast_ttl(ntk_socket *sock, uint8_t ttl);
NTK_API ntk_err_t ntk_socket_set_multicast_if(ntk_socket *sock, const char *local_ip);
NTK_API ntk_err_t ntk_socket_set_multicast_loop(ntk_socket *sock, int on);
NTK_API ntk_err_t ntk_socket_join_multicast(ntk_socket *sock, const char *group_addr,
                                               const char *local_if);
NTK_API ntk_err_t ntk_socket_leave_multicast(ntk_socket *sock, const char *group_addr,
                                                const char *local_if);
NTK_API ntk_err_t ntk_socket_join_multicast_filter(ntk_socket *sock, const char *group_addr,
                                                      const char *src_ip, const char *local_if);
NTK_API ntk_err_t ntk_socket_leave_multicast_filter(ntk_socket *sock, const char *group_addr,
                                                       const char *src_ip, const char *local_if);

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_SOCKET_H */
