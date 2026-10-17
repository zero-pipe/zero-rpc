#ifndef NTK_SSL_CTX_INTERNAL_H
#define NTK_SSL_CTX_INTERNAL_H

#include <openssl/ssl.h>

struct ntk_ssl_ctx {
    SSL_CTX *ctx;
};

#endif
