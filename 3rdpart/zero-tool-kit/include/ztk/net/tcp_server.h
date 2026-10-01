#ifndef ZTK_NET_TCP_SERVER_H
#define ZTK_NET_TCP_SERVER_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_poller_pool;
struct ztk_socket;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_poller_pool ztk_poller_pool;
typedef struct ztk_socket ztk_socket;
typedef struct ztk_tcp_server ztk_tcp_server;
typedef struct ztk_tcp_session ztk_tcp_session;

typedef void (*ztk_tcp_session_out_highwater_cb)(ztk_tcp_session *session, size_t pending, void *user);

typedef struct ztk_tcp_session_ops {
    void (*on_recv)(ztk_tcp_session *session, const void *data, size_t len, void *user);
    void (*on_error)(ztk_tcp_session *session, void *user);
    /** 周期性回调（在所属 poller 线程）；可选 */
    void (*on_manager)(ztk_tcp_session *session, void *user);
} ztk_tcp_session_ops_t;

typedef struct ztk_tcp_server_opts {
    const char *host;
    uint16_t port;
    int backlog;

    /**
     * I/O 线程由 pool 提供（须已 create；start 前调用 ztk_poller_pool_start）。
     * TcpServer 不创建 poller 线程。
     */
    ztk_poller_pool *poller_pool;

    const ztk_tcp_session_ops_t *session_ops;
    void *session_user;
    void *(*session_create_user)(ztk_tcp_server *srv, ztk_tcp_session *session);

    ztk_poller *(*pick_poller)(ztk_tcp_server *srv, ztk_poller *accept_poller, void *user);
    void *pick_poller_user;

    /** 会话巡检间隔（秒）；0 且配置了 on_manager 时默认 2s，<0 表示关闭 */
    float manager_interval_sec;

    /** 出站队列字节高水位；0 表示不回调 */
    size_t out_highwater_bytes;
    ztk_tcp_session_out_highwater_cb on_out_highwater;
} ztk_tcp_server_opts_t;

ZTK_API ztk_tcp_server *ztk_tcp_server_create(const ztk_tcp_server_opts_t *opts);
ZTK_API void ztk_tcp_server_destroy(ztk_tcp_server *srv);

/** 绑定 listen 并注册到 pool 内各 poller；若配置了 on_manager 则挂载巡检 timer */
ZTK_API ztk_err_t ztk_tcp_server_start(ztk_tcp_server *srv);

ZTK_API void ztk_tcp_server_stop(ztk_tcp_server *srv);

ZTK_API int ztk_tcp_server_is_running(const ztk_tcp_server *srv);

ZTK_API uint16_t ztk_tcp_server_port(const ztk_tcp_server *srv);
ZTK_API unsigned ztk_tcp_server_poller_count(const ztk_tcp_server *srv);
ZTK_API ztk_poller *ztk_tcp_server_poller(const ztk_tcp_server *srv, unsigned index);

ZTK_API ztk_socket *ztk_tcp_session_socket(ztk_tcp_session *session);
ZTK_API void *ztk_tcp_session_user(ztk_tcp_session *session);
ZTK_API ztk_poller *ztk_tcp_session_poller(ztk_tcp_session *session);
struct ztk_buf;
typedef struct ztk_buf ztk_buf;

/** 拷贝发送；阻塞/半写时入队缓冲走 session poller 的 alloc_local */
ZTK_API ztk_err_t ztk_tcp_session_send(ztk_tcp_session *session, const void *data, size_t len);
ZTK_API ztk_err_t ztk_tcp_session_sendv(ztk_tcp_session *session,
                                           const void *parts[], const size_t lens[], unsigned count);
/** 零拷贝发送；内部 ztk_buf_ref，写完成后 ztk_buf_unref（调用方宜用 alloc_local） */
ZTK_API ztk_err_t ztk_tcp_session_send_buf(ztk_tcp_session *session, ztk_buf *buf);
ZTK_API void ztk_tcp_session_flush(ztk_tcp_session *session);
ZTK_API size_t ztk_tcp_session_out_pending(const ztk_tcp_session *session);
/** 丢弃尚未发出的应用层发送缓冲（VOD seek 时避免旧媒体排在 Notify 前） */
ZTK_API void ztk_tcp_session_out_discard(ztk_tcp_session *session);
ZTK_API void ztk_tcp_session_close(ztk_tcp_session *session);

#ifdef ZTK_TEST_HOOKS
ZTK_API void ztk_tcp_test_reset_out_mem_append(void);
ZTK_API size_t ztk_tcp_test_out_mem_append_bytes(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_TCP_SERVER_H */
