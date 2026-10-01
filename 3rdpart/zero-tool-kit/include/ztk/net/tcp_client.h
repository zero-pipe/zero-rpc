#ifndef ZTK_NET_TCP_CLIENT_H
#define ZTK_NET_TCP_CLIENT_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include "socket.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_socket;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_socket ztk_socket;
typedef struct ztk_tcp_client ztk_tcp_client;

typedef struct ztk_tcp_client_ops {
    void (*on_connect)(ztk_tcp_client *client, void *user);
    void (*on_recv)(ztk_tcp_client *client, const void *data, size_t len, void *user);
    void (*on_error)(ztk_tcp_client *client, void *user);
} ztk_tcp_client_ops_t;

typedef struct ztk_tcp_client_opts {
    ztk_poller *poller;
    const ztk_tcp_client_ops_t *ops;
    void *user;
} ztk_tcp_client_opts_t;

ZTK_API ztk_tcp_client *ztk_tcp_client_create(const ztk_tcp_client_opts_t *opts);
ZTK_API void ztk_tcp_client_destroy(ztk_tcp_client *client);

/** 非阻塞连接并注册到 poller；成功连通后触发 on_connect */
ZTK_API ztk_err_t ztk_tcp_client_connect(ztk_tcp_client *client, const char *host, uint16_t port);

ZTK_API int ztk_tcp_client_is_connected(const ztk_tcp_client *client);
ZTK_API ztk_socket *ztk_tcp_client_socket(ztk_tcp_client *client);
/** 非阻塞发送；返回已写字节数，全阻塞时 0，出错返回负错误码。调用方需按返回值推进。 */
ZTK_API ztk_ssize_t ztk_tcp_client_send(ztk_tcp_client *client, const void *data, size_t len);
ZTK_API void ztk_tcp_client_close(ztk_tcp_client *client);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_TCP_CLIENT_H */
