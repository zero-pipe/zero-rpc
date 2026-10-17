#ifndef NTK_NET_SSL_H
#define NTK_NET_SSL_H

#include "../ntk_export.h"
#include "../ntk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_ssl_ctx ntk_ssl_ctx;

/** 客户端 TLS 配置（拉流用） */
typedef struct ntk_ssl_config {
    /** PEM CA 文件；空则使用系统默认（若 verify_peer=1 且 OpenSSL 支持） */
    const char *ca_file;
    /** 1=校验服务端证书；0=不校验（仅测试/内网） */
    int verify_peer;
    /** 可选客户端证书 PEM */
    const char *client_cert_file;
    const char *client_key_file;
} ntk_ssl_config_t;

/** 进程级 OpenSSL 初始化（可重复调用，线程安全） */
NTK_API ntk_err_t ntk_ssl_global_init(void);
NTK_API void ntk_ssl_global_fini(void);

NTK_API ntk_ssl_ctx *ntk_ssl_ctx_create(const ntk_ssl_config_t *cfg);
NTK_API void ntk_ssl_ctx_destroy(ntk_ssl_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_SSL_H */
