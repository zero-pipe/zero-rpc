#include "ztk/util/byte_buf.h"
#include <stdlib.h>
#include <string.h>

struct ztk_byte_buf {
    char *data;
    size_t size;
    size_t capacity;
};

ztk_byte_buf *ztk_byte_buf_create(size_t initial_capacity)
{
    ztk_byte_buf *b = (ztk_byte_buf *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    if (initial_capacity > 0) {
        b->data = (char *)malloc(initial_capacity);
        if (!b->data) {
            free(b);
            return NULL;
        }
        b->capacity = initial_capacity;
    }
    return b;
}

void ztk_byte_buf_destroy(ztk_byte_buf *b)
{
    if (!b)
        return;
    free(b->data);
    free(b);
}

const char *ztk_byte_buf_data(const ztk_byte_buf *b)
{
    return b && b->data ? b->data : "";
}

size_t ztk_byte_buf_size(const ztk_byte_buf *b)
{
    return b ? b->size : 0;
}

size_t ztk_byte_buf_capacity(const ztk_byte_buf *b)
{
    return b ? b->capacity : 0;
}

ztk_err_t ztk_byte_buf_reserve(ztk_byte_buf *b, size_t capacity)
{
    if (!b)
        return ZTK_ERR_INVALID;
    if (capacity <= b->capacity)
        return ZTK_OK;
    char *p = (char *)realloc(b->data, capacity);
    if (!p)
        return ZTK_ERR_NOMEM;
    b->data = p;
    b->capacity = capacity;
    return ZTK_OK;
}

ztk_err_t ztk_byte_buf_append(ztk_byte_buf *b, const void *data, size_t len)
{
    if (!b || (!data && len))
        return ZTK_ERR_INVALID;
    if (len == 0)
        return ZTK_OK;

    size_t new_size = b->size + len;
    size_t need_cap = new_size + 1;
    if (need_cap > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 64;
        while (cap < need_cap)
            cap *= 2;
        ztk_err_t err = ztk_byte_buf_reserve(b, cap);
        if (err != ZTK_OK)
            return err;
    }
    memcpy(b->data + b->size, data, len);
    b->size = new_size;
    b->data[b->size] = '\0';
    return ZTK_OK;
}

ztk_err_t ztk_byte_buf_clear(ztk_byte_buf *b)
{
    if (!b)
        return ZTK_ERR_INVALID;
    b->size = 0;
    if (b->data)
        b->data[0] = '\0';
    return ZTK_OK;
}

ztk_err_t ztk_byte_buf_erase_front(ztk_byte_buf *b, size_t n)
{
    if (!b)
        return ZTK_ERR_INVALID;
    if (n == 0)
        return ZTK_OK;
    if (n >= b->size) {
        b->size = 0;
        if (b->data)
            b->data[0] = '\0';
        return ZTK_OK;
    }
    memmove(b->data, b->data + n, b->size - n);
    b->size -= n;
    if (b->data)
        b->data[b->size] = '\0';
    return ZTK_OK;
}
