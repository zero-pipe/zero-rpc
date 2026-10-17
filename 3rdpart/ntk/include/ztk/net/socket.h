#ifndef ZTK_NET_SOCKET_H
#define ZTK_NET_SOCKET_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#  include <BaseTsd.h>
typedef SSIZE_T ztk_ssize_t;
#elif defined(__GNUC__) || defined(__linux__)
#  include <sys/types.h>
typedef ssize_t ztk_ssize_t;
#else
typedef ptrdiff_t ztk_ssize_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_socket ztk_socket;

typedef enum ztk_sock_type {
    ZTK_SOCK_NONE = 0,
    ZTK_SOCK_TCP_CLIENT,
    ZTK_SOCK_TCP_LISTEN,
    ZTK_SOCK_TCP_CONNECTED,
    ZTK_SOCK_UDP
} ztk_sock_type_t;

/**
 * Poller 就绪回调（在 poll 线程内调用，勿阻塞）。
 * on_readable：可读（TCP 已连接 / UDP / listen 可 accept）
 * on_writable：可写（含异步 connect 完成前需监听 OUT）
 * on_error：错误或对端关闭（EPOLLERR/HUP）
 */
typedef struct ztk_socket_callbacks {
    void (*on_readable)(ztk_socket *sock, void *user);
    void (*on_writable)(ztk_socket *sock, void *user);
    void (*on_error)(ztk_socket *sock, void *user);
} ztk_socket_callbacks_t;

ZTK_API ztk_socket *ztk_socket_create(void);
ZTK_API void ztk_socket_destroy(ztk_socket *sock);

/** 接管已有 fd（设为非阻塞 + CLOEXEC），成功后由 ztk_socket 负责 close */
ZTK_API ztk_err_t ztk_socket_adopt(ztk_socket *sock, int fd, ztk_sock_type_t type);

ZTK_API int ztk_socket_fd(const ztk_socket *sock);
ZTK_API ztk_sock_type_t ztk_socket_type(const ztk_socket *sock);

/** host 可为 NULL / "" / "0.0.0.0"；port=0 表示由系统分配 */
ZTK_API ztk_err_t ztk_socket_listen(ztk_socket *sock, const char *host, uint16_t port, int backlog);

/** @param reuse SO_REUSEADDR */
ZTK_API ztk_err_t ztk_socket_bind_udp(ztk_socket *sock, const char *host, uint16_t port, int reuse);

/** reuse_addr / reuse_port 对应 SO_REUSEADDR / SO_REUSEPORT */
ZTK_API ztk_err_t ztk_socket_bind_udp_ex(ztk_socket *sock, const char *host, uint16_t port,
                                            int reuse_addr, int reuse_port);

/**
 * TCP 连接。已连接返回 ZTK_OK；非阻塞进行中也返回 ZTK_OK 且 *in_progress=1。
 * 失败返回负错误码。
 */
ZTK_API ztk_err_t ztk_socket_connect(ztk_socket *sock, const char *host, uint16_t port, int *in_progress);

/** 异步 connect 后，在 on_writable 或 poll OUT 后调用，检查是否真正连通 */
ZTK_API ztk_err_t ztk_socket_check_connect(ztk_socket *sock);

/** 从监听 socket accept，*out_client 为新分配的 socket */
ZTK_API ztk_err_t ztk_socket_accept(ztk_socket *listen_sock, ztk_socket **out_client);

typedef struct ztk_socket_iov {
    const void *base;
    size_t len;
} ztk_socket_iov;

#define ZTK_SOCKET_IOV_MAX 16

ZTK_API ztk_ssize_t ztk_socket_send(ztk_socket *sock, const void *data, size_t len);
ZTK_API ztk_ssize_t ztk_socket_sendv(ztk_socket *sock, const ztk_socket_iov *iov, unsigned count);

/** Linux 可选 BBR；Win32 返回 ZTK_ERR_NOT_IMPL */
ZTK_API ztk_err_t ztk_socket_set_bbr(ztk_socket *sock, int on);
ZTK_API ztk_ssize_t ztk_socket_recv(ztk_socket *sock, void *buf, size_t len);

ZTK_API ztk_ssize_t ztk_socket_sendto(ztk_socket *sock, const void *data, size_t len,
                                         const char *ip, uint16_t port);
ZTK_API ztk_ssize_t ztk_socket_recvfrom(ztk_socket *sock, void *buf, size_t len,
                                           char *ip, size_t ip_len, uint16_t *port);

/** UDP 批量发送元素。 */
typedef struct ztk_udp_dgram {
    const void *data;
    size_t len;
    const char *ip;
    uint16_t port;
} ztk_udp_dgram_t;

/** UDP 批量接收元素（data/cap 入参，len/ip/port 出参）。 */
typedef struct ztk_udp_recv {
    void *data;
    size_t cap;
    size_t len;
    char ip[46];
    uint16_t port;
} ztk_udp_recv_t;

/**
 * 批量发送多个 UDP 数据报（Linux: sendmmsg，一次系统调用）。
 * @return 实际发送条数（>=0），或负错误码。
 */
ZTK_API int ztk_socket_sendto_batch(ztk_socket *sock, const ztk_udp_dgram_t *msgs, unsigned count);

/**
 * 批量接收多个 UDP 数据报（Linux: recvmmsg，一次系统调用）。
 * @param msgs 数组，每项 data/cap 由调用方提供
 * @return 实际接收条数（>=0，0 表示无更多数据），或负错误码。
 */
ZTK_API int ztk_socket_recvfrom_batch(ztk_socket *sock, ztk_udp_recv_t *msgs, unsigned cap);

ZTK_API ztk_err_t ztk_socket_get_local(const ztk_socket *sock, char *ip, size_t ip_len, uint16_t *port);
ZTK_API ztk_err_t ztk_socket_get_peer(const ztk_socket *sock, char *ip, size_t ip_len, uint16_t *port);

/** 注册到 poller；listen 默认 IN，connect 进行中会附加 OUT */
ZTK_API ztk_err_t ztk_socket_attach_poller(ztk_socket *sock, ztk_poller *poller,
                                              const ztk_socket_callbacks_t *cb, void *user);
ZTK_API ztk_err_t ztk_socket_detach_poller(ztk_socket *sock);

/** 修改关注事件（已 attach 后） */
ZTK_API ztk_err_t ztk_socket_mod_events(ztk_socket *sock, unsigned events);

/** UDP：开启广播发送（SO_BROADCAST） */
ZTK_API ztk_err_t ztk_socket_set_broadcast(ztk_socket *sock, int on);

ZTK_API ztk_err_t ztk_socket_set_multicast_ttl(ztk_socket *sock, uint8_t ttl);
ZTK_API ztk_err_t ztk_socket_set_multicast_if(ztk_socket *sock, const char *local_ip);
ZTK_API ztk_err_t ztk_socket_set_multicast_loop(ztk_socket *sock, int on);
ZTK_API ztk_err_t ztk_socket_join_multicast(ztk_socket *sock, const char *group_addr,
                                               const char *local_if);
ZTK_API ztk_err_t ztk_socket_leave_multicast(ztk_socket *sock, const char *group_addr,
                                                const char *local_if);
ZTK_API ztk_err_t ztk_socket_join_multicast_filter(ztk_socket *sock, const char *group_addr,
                                                      const char *src_ip, const char *local_if);
ZTK_API ztk_err_t ztk_socket_leave_multicast_filter(ztk_socket *sock, const char *group_addr,
                                                       const char *src_ip, const char *local_if);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_SOCKET_H */
