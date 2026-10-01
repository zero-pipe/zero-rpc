#include "ztk/net/ssl.h"
#include "internal/ssl_ctx_internal.h"
#if defined(_WIN32)
#  include <winsock2.h>
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdlib.h>
#include <string.h>

static int s_ssl_inited;
static int s_ssl_init_count;

static int ssl_verify_cb(int ok, X509_STORE_CTX *store)
{
    (void)store;
    return ok;
}

ztk_err_t ztk_ssl_global_init(void)
{
    if (s_ssl_inited)
        return ZTK_OK;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL) != 1) {
        ztk_set_last_error("OPENSSL_init_ssl failed");
        return ZTK_ERR_PLATFORM;
    }
#else
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif
    s_ssl_inited = 1;
    s_ssl_init_count = 0;
    return ZTK_OK;
}

void ztk_ssl_global_fini(void)
{
    if (!s_ssl_inited || s_ssl_init_count > 0)
        return;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    /* OpenSSL 1.1+ 通常无需 EVP_cleanup */
#else
    EVP_cleanup();
#endif
    s_ssl_inited = 0;
}

ztk_ssl_ctx *ztk_ssl_ctx_create(const ztk_ssl_config_t *cfg)
{
    if (ztk_ssl_global_init() != ZTK_OK)
        return NULL;

    ztk_ssl_ctx *sc = (ztk_ssl_ctx *)calloc(1, sizeof(*sc));
    if (!sc)
        return NULL;

    const SSL_METHOD *method = TLS_client_method();
    sc->ctx = SSL_CTX_new(method);
    if (!sc->ctx) {
        free(sc);
        ztk_set_last_error("SSL_CTX_new failed");
        return NULL;
    }

    SSL_CTX_set_options(sc->ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    SSL_CTX_set_min_proto_version(sc->ctx, TLS1_2_VERSION);
#endif

    int verify = (cfg && cfg->verify_peer) ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(sc->ctx, verify, ssl_verify_cb);

    if (cfg && cfg->ca_file && cfg->ca_file[0]) {
        if (SSL_CTX_load_verify_locations(sc->ctx, cfg->ca_file, NULL) != 1) {
            ztk_set_last_error("SSL_CTX_load_verify_locations failed");
            SSL_CTX_free(sc->ctx);
            free(sc);
            return NULL;
        }
    } else if (cfg && cfg->verify_peer) {
        if (SSL_CTX_set_default_verify_paths(sc->ctx) != 1) {
            ztk_set_last_error("SSL_CTX_set_default_verify_paths failed");
            SSL_CTX_free(sc->ctx);
            free(sc);
            return NULL;
        }
    }

    if (cfg && cfg->client_cert_file && cfg->client_cert_file[0]) {
        if (SSL_CTX_use_certificate_file(sc->ctx, cfg->client_cert_file, SSL_FILETYPE_PEM) != 1) {
            ztk_set_last_error("SSL_CTX_use_certificate_file failed");
            SSL_CTX_free(sc->ctx);
            free(sc);
            return NULL;
        }
    }
    if (cfg && cfg->client_key_file && cfg->client_key_file[0]) {
        if (SSL_CTX_use_PrivateKey_file(sc->ctx, cfg->client_key_file, SSL_FILETYPE_PEM) != 1) {
            ztk_set_last_error("SSL_CTX_use_PrivateKey_file failed");
            SSL_CTX_free(sc->ctx);
            free(sc);
            return NULL;
        }
        if (SSL_CTX_check_private_key(sc->ctx) != 1) {
            ztk_set_last_error("SSL_CTX_check_private_key failed");
            SSL_CTX_free(sc->ctx);
            free(sc);
            return NULL;
        }
    }

    ++s_ssl_init_count;
    return sc;
}

void ztk_ssl_ctx_destroy(ztk_ssl_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->ctx)
        SSL_CTX_free(ctx->ctx);
    free(ctx);
    if (s_ssl_init_count > 0)
        --s_ssl_init_count;
}
