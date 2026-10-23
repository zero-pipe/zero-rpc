#ifndef NTK_NET_TLS_CLIENT_H
#define NTK_NET_TLS_CLIENT_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
struct ntk_ssl_ctx;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_ssl_ctx ntk_ssl_ctx;
typedef struct ntk_tls_client ntk_tls_client;

typedef struct ntk_tls_client_ops {
    void (*on_connect)(ntk_tls_client *client, void *user);
    void (*on_recv)(ntk_tls_client *client, const void *data, size_t len, void *user);
    void (*on_error)(ntk_tls_client *client, void *user);
} ntk_tls_client_ops_t;

typedef struct ntk_tls_client_opts {
    ntk_poller *poller;
    ntk_ssl_ctx *ssl_ctx;
    const ntk_tls_client_ops_t *ops;
    void *user;
    /**
     * TLS SNI / 校验用主机名；NULL 则使用 connect 的 host。
     */
    const char *sni_host;
} ntk_tls_client_opts_t;

NTK_API ntk_tls_client *ntk_tls_client_create(const ntk_tls_client_opts_t *opts);
NTK_API void ntk_tls_client_destroy(ntk_tls_client *client);

/** TCP 连通并完成 TLS 握手后触发 on_connect */
NTK_API ntk_err_t ntk_tls_client_connect(ntk_tls_client *client, const char *host, uint16_t port);

NTK_API int ntk_tls_client_is_connected(const ntk_tls_client *client);
NTK_API ntk_err_t ntk_tls_client_send(ntk_tls_client *client, const void *data, size_t len);
NTK_API void ntk_tls_client_close(ntk_tls_client *client);

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_TLS_CLIENT_H */
