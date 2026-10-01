#include "ztk/util/byte_buf.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    ztk_byte_buf *b = ztk_byte_buf_create(8);
    if (!b)
        return 1;

    if (ztk_byte_buf_append(b, "hello", 5) != ZTK_OK)
        return 1;
    if (ztk_byte_buf_append(b, " world", 6) != ZTK_OK)
        return 1;

    if (strcmp(ztk_byte_buf_data(b), "hello world") != 0) {
        fprintf(stderr, "bad data: %s\n", ztk_byte_buf_data(b));
        ztk_byte_buf_destroy(b);
        return 1;
    }

    if (ztk_byte_buf_size(b) != 11)
        return 1;

    ztk_byte_buf_clear(b);
    if (ztk_byte_buf_size(b) != 0)
        return 1;

    ztk_byte_buf_destroy(b);
    printf("test_byte_buf ok\n");
    return 0;
}
