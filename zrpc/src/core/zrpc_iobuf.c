#include "zrpc_iobuf.h"
#include "zrpc_mem.h"

#include <stdlib.h>
#include <string.h>

static zrpc_iobuf_blk_t *blk_new(ztk_buf *owner, const void *data, size_t len) {
    zrpc_iobuf_blk_t *blk;

    if (!data || len == 0) {
        return NULL;
    }
    blk = (zrpc_iobuf_blk_t *)zrpc_mem_alloc(sizeof(*blk));
    if (!blk) {
        return NULL;
    }
    blk->next = NULL;
    blk->owner = owner;
    blk->data = (const uint8_t *)data;
    blk->off = 0;
    blk->len = len;
    return blk;
}

static void blk_release(zrpc_iobuf_blk_t *blk) {
    if (!blk) {
        return;
    }
    if (blk->owner) {
        ztk_buf_unref(blk->owner);
    }
    zrpc_mem_free(blk);
}

static int iobuf_push(zrpc_iobuf_t *b, zrpc_iobuf_blk_t *blk) {
    if (!b || !blk) {
        return -1;
    }
    if (b->tail) {
        b->tail->next = blk;
    } else {
        b->head = blk;
    }
    b->tail = blk;
    b->total += blk->len;
    return 0;
}

void zrpc_iobuf_init(zrpc_iobuf_t *b) {
    if (!b) {
        return;
    }
    b->head = NULL;
    b->tail = NULL;
    b->total = 0;
}

void zrpc_iobuf_reset(zrpc_iobuf_t *b) {
    zrpc_iobuf_blk_t *blk;

    if (!b) {
        return;
    }
    blk = b->head;
    while (blk) {
        zrpc_iobuf_blk_t *next = blk->next;
        blk_release(blk);
        blk = next;
    }
    zrpc_iobuf_init(b);
}

size_t zrpc_iobuf_len(const zrpc_iobuf_t *b) {
    return b ? b->total : 0;
}

int zrpc_iobuf_empty(const zrpc_iobuf_t *b) {
    return (!b || b->total == 0) ? 1 : 0;
}

int zrpc_iobuf_append_copy(zrpc_iobuf_t *b, ztk_poller *poller, const void *data, size_t len) {
    ztk_buf *owner;
    zrpc_iobuf_blk_t *blk;

    if (!b || !data || len == 0) {
        return -1;
    }
    owner = ztk_buf_alloc_local(poller, len);
    if (!owner) {
        return -1;
    }
    memcpy((void *)ztk_buf_data(owner), data, len);
    ztk_buf_set_len(owner, len);
    blk = blk_new(owner, ztk_buf_data(owner), len);
    if (!blk) {
        ztk_buf_unref(owner);
        return -1;
    }
    blk->owner = owner; /* blk_new 已赋值，这里显式强调所有权 */
    if (iobuf_push(b, blk) != 0) {
        blk_release(blk);
        return -1;
    }
    return 0;
}

int zrpc_iobuf_append_buf(zrpc_iobuf_t *b, ztk_buf *owner, const void *data, size_t len) {
    zrpc_iobuf_blk_t *blk;

    if (!b || !owner || !data || len == 0) {
        return -1;
    }
    ztk_buf_ref(owner); /* block 持有一份引用 */
    blk = blk_new(owner, data, len);
    if (!blk) {
        ztk_buf_unref(owner);
        return -1;
    }
    if (iobuf_push(b, blk) != 0) {
        blk_release(blk);
        return -1;
    }
    return 0;
}

int zrpc_iobuf_append_borrow(zrpc_iobuf_t *b, const void *data, size_t len) {
    zrpc_iobuf_blk_t *blk;

    if (!b || !data || len == 0) {
        return -1;
    }
    blk = blk_new(NULL, data, len); /* 无主内存：借用，不参与引用计数 */
    if (!blk) {
        return -1;
    }
    if (iobuf_push(b, blk) != 0) {
        blk_release(blk);
        return -1;
    }
    return 0;
}

static const zrpc_iobuf_blk_t *blk_at_offset(const zrpc_iobuf_t *b, size_t off, size_t *blk_off) {
    const zrpc_iobuf_blk_t *blk = b->head;

    while (blk) {
        size_t remain = blk->len - blk->off;
        if (off < remain) {
            if (blk_off) {
                *blk_off = blk->off + off;
            }
            return blk;
        }
        off -= remain;
        blk = blk->next;
    }
    return NULL;
}

size_t zrpc_iobuf_peek(const zrpc_iobuf_t *b, size_t off, void *dst, size_t n) {
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;

    if (!b || !dst || n == 0) {
        return 0;
    }
    while (done < n && b->total > off) {
        size_t blk_off = 0;
        const zrpc_iobuf_blk_t *blk = blk_at_offset(b, off, &blk_off);
        size_t remain;
        size_t take;
        if (!blk) {
            break;
        }
        remain = blk->len - blk_off;
        take = remain;
        if (take > n - done) {
            take = n - done;
        }
        if (blk->data && take > 0) {
            memcpy(out + done, blk->data + blk_off, take);
        }
        done += take;
        off += take;
    }
    return done;
}

unsigned zrpc_iobuf_iovec(const zrpc_iobuf_t *b, ztk_socket_iov *iov, unsigned max_count,
                          size_t max_bytes) {
    const zrpc_iobuf_blk_t *blk;
    unsigned n = 0;
    size_t total = 0;

    if (!b || !iov || max_count == 0) {
        return 0;
    }
    for (blk = b->head; blk && n < max_count; blk = blk->next) {
        size_t remain = blk->len - blk->off;
        if (remain == 0) {
            continue;
        }
        if (max_bytes && total + remain > max_bytes) {
            remain = max_bytes - total;
            if (remain == 0) {
                break;
            }
        }
        iov[n].base = blk->data + blk->off;
        iov[n].len = remain;
        total += remain;
        n++;
        if (max_bytes && total >= max_bytes) {
            break;
        }
    }
    return n;
}

void zrpc_iobuf_consume(zrpc_iobuf_t *b, size_t n) {
    if (!b || n == 0) {
        return;
    }
    while (n > 0 && b->head) {
        zrpc_iobuf_blk_t *blk = b->head;
        size_t remain = blk->len - blk->off;
        if (n < remain) {
            blk->off += n;
            b->total -= n;
            return;
        }
        /* 整块消费完：释放并前进 */
        n -= remain;
        b->total -= remain;
        b->head = blk->next;
        if (!b->head) {
            b->tail = NULL;
        }
        blk_release(blk);
    }
}

const uint8_t *zrpc_iobuf_view(zrpc_iobuf_t *b, size_t n, ztk_poller *poller, ztk_buf **scratch) {
    const zrpc_iobuf_blk_t *blk;
    size_t head_remain;

    if (!b || n == 0) {
        return NULL;
    }
    blk = b->head;
    if (!blk) {
        return NULL;
    }
    head_remain = blk->len - blk->off;
    if (head_remain >= n) {
        /* 连续：直接指向内部内存，不拷贝 */
        return blk->data + blk->off;
    }
    /* 跨块：拷到 scratch（保证 payload 视图连续，兼容扁平 payload API） */
    if (!scratch) {
        return NULL;
    }
    if (!*scratch || ztk_buf_cap(*scratch) < n) {
        ztk_buf *nb = ztk_buf_alloc_local(poller, n);
        if (!nb) {
            return NULL;
        }
        if (*scratch) {
            ztk_buf_unref(*scratch);
        }
        *scratch = nb;
    }
    if (zrpc_iobuf_peek(b, 0, (void *)ztk_buf_data(*scratch), n) != n) {
        return NULL;
    }
    ztk_buf_set_len(*scratch, n);
    return ztk_buf_data(*scratch);
}
