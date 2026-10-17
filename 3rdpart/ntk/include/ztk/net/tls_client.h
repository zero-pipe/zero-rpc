#ifndef ZTK_NET_TLS_CLIENT_H
#define ZTK_NET_TLS_CLIENT_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_ssl_ctx;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_ssl_ctx ztk_ssl_ctx;
typedef struct ztk_tls_client ztk_tls_client;

typedef struct ztk_tls_client_ops {
    void (*on_connect)(ztk_tls_client *client, void *user);
    void (*on_recv)(ztk_tls_client *client, const void *data, size_t len, void *user);
    void (*on_error)(ztk_tls_client *client, void *user);
} ztk_tls_client_ops_t;

typedef struct ztk_tls_client_opts {
    ztk_poller *poller;
    ztk_ssl_ctx *ssl_ctx;
    const ztk_tls_client_ops_t *ops;
    void *user;
    /**
     * TLS SNI / 校验用主机名；NULL 则使用 connect 的 host。
     */
    const char *sni_host;
} ztk_tls_client_opts_t;

ZTK_API ztk_tls_client *ztk_tls_client_create(const ztk_tls_client_opts_t *opts);
ZTK_API void ztk_tls_client_destroy(ztk_tls_client *client);

/** TCP 连通并完成 TLS 握手后触发 on_connect */
ZTK_API ztk_err_t ztk_tls_client_connect(ztk_tls_client *client, const char *host, uint16_t port);

ZTK_API int ztk_tls_client_is_connected(const ztk_tls_client *client);
ZTK_API ztk_err_t ztk_tls_client_send(ztk_tls_client *client, const void *data, size_t len);
ZTK_API void ztk_tls_client_close(ztk_tls_client *client);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_TLS_CLIENT_H */
