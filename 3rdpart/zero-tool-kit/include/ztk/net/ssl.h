#ifndef ZTK_NET_SSL_H
#define ZTK_NET_SSL_H

#include "../ztk_export.h"
#include "../ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_ssl_ctx ztk_ssl_ctx;

/** 客户端 TLS 配置（拉流用） */
typedef struct ztk_ssl_config {
    /** PEM CA 文件；空则使用系统默认（若 verify_peer=1 且 OpenSSL 支持） */
    const char *ca_file;
    /** 1=校验服务端证书；0=不校验（仅测试/内网） */
    int verify_peer;
    /** 可选客户端证书 PEM */
    const char *client_cert_file;
    const char *client_key_file;
} ztk_ssl_config_t;

/** 进程级 OpenSSL 初始化（可重复调用，线程安全） */
ZTK_API ztk_err_t ztk_ssl_global_init(void);
ZTK_API void ztk_ssl_global_fini(void);

ZTK_API ztk_ssl_ctx *ztk_ssl_ctx_create(const ztk_ssl_config_t *cfg);
ZTK_API void ztk_ssl_ctx_destroy(ztk_ssl_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_SSL_H */
