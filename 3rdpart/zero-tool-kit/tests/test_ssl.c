#include "ztk/net/ssl.h"
#include "ztk/ztk_errno.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
#if !defined(ZTK_HAVE_OPENSSL)
    printf("skip: ZTK_HAVE_OPENSSL not defined\n");
    return 0;
#else
    if (ztk_ssl_global_init() != ZTK_OK) {
        fprintf(stderr, "ztk_ssl_global_init failed\n");
        return 1;
    }

    ztk_ssl_config_t cfg = { .ca_file = NULL, .verify_peer = 0 };
    ztk_ssl_ctx *ctx = ztk_ssl_ctx_create(&cfg);
    if (!ctx) {
        fprintf(stderr, "ztk_ssl_ctx_create failed: %s\n", ztk_last_error());
        return 1;
    }

    ztk_ssl_config_t bad = { .ca_file = "___ztk_no_such_ca_file___.pem", .verify_peer = 1 };
    if (ztk_ssl_ctx_create(&bad) != NULL) {
        fprintf(stderr, "expected CA load failure\n");
        ztk_ssl_ctx_destroy(ctx);
        return 1;
    }

    ztk_ssl_ctx_destroy(ctx);
    ztk_ssl_global_fini();
    printf("test_ssl ok\n");
    return 0;
#endif
}
