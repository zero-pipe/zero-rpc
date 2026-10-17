#ifndef ZTK_SSL_CTX_INTERNAL_H
#define ZTK_SSL_CTX_INTERNAL_H

#include <openssl/ssl.h>

struct ztk_ssl_ctx {
    SSL_CTX *ctx;
};

#endif
