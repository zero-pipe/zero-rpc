#include "zrpc_iobuf.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_fail = 1;                                                    \
        }                                                                  \
    } while (0)

static void test_copy_peek_consume(void) {
    zrpc_iobuf_t b;
    const char a[] = "hello";
    const char c[] = " world";
    char out[32];
    ztk_socket_iov iov[4];

    zrpc_iobuf_init(&b);
    CHECK(zrpc_iobuf_append_copy(&b, NULL, a, sizeof(a) - 1) == 0);
    CHECK(zrpc_iobuf_append_copy(&b, NULL, c, sizeof(c) - 1) == 0);
    CHECK(zrpc_iobuf_len(&b) == 11);
    memset(out, 0, sizeof(out));
    CHECK(zrpc_iobuf_peek(&b, 0, out, 11) == 11);
    CHECK(memcmp(out, "hello world", 11) == 0);
    CHECK(zrpc_iobuf_iovec(&b, iov, 4, 0) == 2);
    CHECK(iov[0].len == 5 && iov[1].len == 6);
    zrpc_iobuf_consume(&b, 5);
    CHECK(zrpc_iobuf_len(&b) == 6);
    memset(out, 0, sizeof(out));
    CHECK(zrpc_iobuf_peek(&b, 0, out, 6) == 6);
    CHECK(memcmp(out, " world", 6) == 0);
    zrpc_iobuf_consume(&b, 100);
    CHECK(zrpc_iobuf_empty(&b));
    zrpc_iobuf_reset(&b);
}

static void test_ref_and_borrow(void) {
    zrpc_iobuf_t b;
    ztk_buf *owner;
    const char borrowed[] = "borrowed";
    char out[32];

    zrpc_iobuf_init(&b);
    owner = ztk_buf_alloc(16);
    CHECK(owner != NULL);
    if (!owner) {
        return;
    }
    memcpy((void *)ztk_buf_data(owner), "owned", 5);
    ztk_buf_set_len(owner, 5);
    CHECK(ztk_buf_refcnt(owner) == 1);
    CHECK(zrpc_iobuf_append_buf(&b, owner, ztk_buf_data(owner), 5) == 0);
    CHECK(ztk_buf_refcnt(owner) == 2);
    ztk_buf_unref(owner);
    CHECK(zrpc_iobuf_append_borrow(&b, borrowed, sizeof(borrowed) - 1) == 0);
    memset(out, 0, sizeof(out));
    CHECK(zrpc_iobuf_peek(&b, 0, out, 13) == 13);
    CHECK(memcmp(out, "ownedborrowed", 13) == 0);
    zrpc_iobuf_reset(&b);
}

static void test_view(void) {
    zrpc_iobuf_t b;
    ztk_buf *scratch = NULL;
    const uint8_t *view;
    const char first[] = "abc";
    const char second[] = "def";

    zrpc_iobuf_init(&b);
    CHECK(zrpc_iobuf_append_copy(&b, NULL, first, 3) == 0);
    view = zrpc_iobuf_view(&b, 3, NULL, &scratch);
    CHECK(view != NULL);
    CHECK(memcmp(view, "abc", 3) == 0);
    CHECK(scratch == NULL); /* 单块连续视图不需要 scratch */
    CHECK(zrpc_iobuf_append_copy(&b, NULL, second, 3) == 0);
    view = zrpc_iobuf_view(&b, 6, NULL, &scratch);
    CHECK(view != NULL);
    CHECK(scratch != NULL);
    CHECK(memcmp(view, "abcdef", 6) == 0);
    if (scratch) {
        ztk_buf_unref(scratch);
    }
    zrpc_iobuf_reset(&b);
}

int main(void) {
    test_copy_peek_consume();
    test_ref_and_borrow();
    test_view();
    printf(g_fail ? "IOBUF FAIL\n" : "IOBUF OK\n");
    return g_fail ? 1 : 0;
}
