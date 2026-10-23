#ifndef NTK_NET_TCP_SERVER_H
#define NTK_NET_TCP_SERVER_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
struct ntk_poller_pool;
struct ntk_socket;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_poller_pool ntk_poller_pool;
typedef struct ntk_socket ntk_socket;
typedef struct ntk_tcp_server ntk_tcp_server;
typedef struct ntk_tcp_session ntk_tcp_session;

typedef void (*ntk_tcp_session_out_highwater_cb)(ntk_tcp_session *session, size_t pending, void *user);

typedef struct ntk_tcp_session_ops {
    void (*on_recv)(ntk_tcp_session *session, const void *data, size_t len, void *user);
    void (*on_error)(ntk_tcp_session *session, void *user);
    /** 周期性回调（在所属 poller 线程）；可选 */
    void (*on_manager)(ntk_tcp_session *session, void *user);
} ntk_tcp_session_ops_t;

typedef struct ntk_tcp_server_opts {
    const char *host;
    uint16_t port;
    int backlog;

    /**
     * I/O 线程由 pool 提供（须已 create；start 前调用 ntk_poller_pool_start）。
     * TcpServer 不创建 poller 线程。
     */
    ntk_poller_pool *poller_pool;

    const ntk_tcp_session_ops_t *session_ops;
    void *session_user;
    void *(*session_create_user)(ntk_tcp_server *srv, ntk_tcp_session *session);

    ntk_poller *(*pick_poller)(ntk_tcp_server *srv, ntk_poller *accept_poller, void *user);
    void *pick_poller_user;

    /** 会话巡检间隔（秒）；0 且配置了 on_manager 时默认 2s，<0 表示关闭 */
    float manager_interval_sec;

    /** 出站队列字节高水位；0 表示不回调 */
    size_t out_highwater_bytes;
    ntk_tcp_session_out_highwater_cb on_out_highwater;
} ntk_tcp_server_opts_t;

NTK_API ntk_tcp_server *ntk_tcp_server_create(const ntk_tcp_server_opts_t *opts);
NTK_API void ntk_tcp_server_destroy(ntk_tcp_server *srv);

/** 绑定 listen 并注册到 pool 内各 poller；若配置了 on_manager 则挂载巡检 timer */
NTK_API ntk_err_t ntk_tcp_server_start(ntk_tcp_server *srv);

NTK_API void ntk_tcp_server_stop(ntk_tcp_server *srv);

NTK_API int ntk_tcp_server_is_running(const ntk_tcp_server *srv);

NTK_API uint16_t ntk_tcp_server_port(const ntk_tcp_server *srv);
NTK_API unsigned ntk_tcp_server_poller_count(const ntk_tcp_server *srv);
NTK_API ntk_poller *ntk_tcp_server_poller(const ntk_tcp_server *srv, unsigned index);

NTK_API ntk_socket *ntk_tcp_session_socket(ntk_tcp_session *session);
NTK_API void *ntk_tcp_session_user(ntk_tcp_session *session);
NTK_API ntk_poller *ntk_tcp_session_poller(ntk_tcp_session *session);
struct ntk_buf;
typedef struct ntk_buf ntk_buf;

/** 拷贝发送；阻塞/半写时入队缓冲走 session poller 的 alloc_local */
NTK_API ntk_err_t ntk_tcp_session_send(ntk_tcp_session *session, const void *data, size_t len);
NTK_API ntk_err_t ntk_tcp_session_sendv(ntk_tcp_session *session,
                                           const void *parts[], const size_t lens[], unsigned count);
/** 零拷贝发送；内部 ntk_buf_ref，写完成后 ntk_buf_unref（调用方宜用 alloc_local） */
NTK_API ntk_err_t ntk_tcp_session_send_buf(ntk_tcp_session *session, ntk_buf *buf);
NTK_API void ntk_tcp_session_flush(ntk_tcp_session *session);
NTK_API size_t ntk_tcp_session_out_pending(const ntk_tcp_session *session);
/** 丢弃尚未发出的应用层发送缓冲（VOD seek 时避免旧媒体排在 Notify 前） */
NTK_API void ntk_tcp_session_out_discard(ntk_tcp_session *session);
NTK_API void ntk_tcp_session_close(ntk_tcp_session *session);

#ifdef NTK_TEST_HOOKS
NTK_API void ntk_tcp_test_reset_out_mem_append(void);
NTK_API size_t ntk_tcp_test_out_mem_append_bytes(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_TCP_SERVER_H */
