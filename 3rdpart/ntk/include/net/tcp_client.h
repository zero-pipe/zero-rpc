#ifndef NTK_NET_TCP_CLIENT_H
#define NTK_NET_TCP_CLIENT_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include "socket.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
struct ntk_socket;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_socket ntk_socket;
typedef struct ntk_tcp_client ntk_tcp_client;

typedef struct ntk_tcp_client_ops {
    void (*on_connect)(ntk_tcp_client *client, void *user);
    void (*on_recv)(ntk_tcp_client *client, const void *data, size_t len, void *user);
    void (*on_error)(ntk_tcp_client *client, void *user);
} ntk_tcp_client_ops_t;

typedef struct ntk_tcp_client_opts {
    ntk_poller *poller;
    const ntk_tcp_client_ops_t *ops;
    void *user;
} ntk_tcp_client_opts_t;

NTK_API ntk_tcp_client *ntk_tcp_client_create(const ntk_tcp_client_opts_t *opts);
NTK_API void ntk_tcp_client_destroy(ntk_tcp_client *client);

/** 非阻塞连接并注册到 poller；成功连通后触发 on_connect */
NTK_API ntk_err_t ntk_tcp_client_connect(ntk_tcp_client *client, const char *host, uint16_t port);

NTK_API int ntk_tcp_client_is_connected(const ntk_tcp_client *client);
NTK_API ntk_socket *ntk_tcp_client_socket(ntk_tcp_client *client);
/** 非阻塞发送；返回已写字节数，全阻塞时 0，出错返回负错误码。调用方需按返回值推进。 */
NTK_API ntk_ssize_t ntk_tcp_client_send(ntk_tcp_client *client, const void *data, size_t len);
NTK_API void ntk_tcp_client_close(ntk_tcp_client *client);

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_TCP_CLIENT_H */
