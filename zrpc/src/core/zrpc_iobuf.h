#ifndef ZRPC_IOBUF_H
#define ZRPC_IOBUF_H

/*
 * 引用计数多段缓冲。
 *
 * 传输层的统一数据载体：由若干 block 串成有序字节流，每个 block 要么持有一个
 * ztk_buf（引用计数、可池化），要么是借用/无主内存。提供：
 *   - append：拷贝入池化块 / 引用已有 ztk_buf / 借用外部内存
 *   - peek  ：不消费地读取前缀
 *   - iovec ：把前缀导出为 ztk_socket_iov，直接喂 sendv/sendmsg
 *   - consume：推进前缀消费游标，整块消费完自动 unref
 *
 * 所有权约定（single-reader）：一个 iobuf 只有唯一消费者，通过 consume 推进；
 * 需要共享同一段字节时用 ztk_buf_ref 复制 block，而不是拷贝数据。
 *
 * 线程约定：与 ztk_buf 一致——池化块应在所属 poller 线程内创建/释放；
 * 跨线程释放由 ztk_buf_pool 的 owner 回投机制兜底。
 */

#include <ztk/ztk.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_iobuf_blk {
    struct zrpc_iobuf_blk *next;
    ztk_buf *owner; /* 引用计数块；NULL 表示借用/无主内存 */
    const uint8_t *data;
    size_t off; /* 已消费字节（相对 data） */
    size_t len; /* 有效字节 */
} zrpc_iobuf_blk_t;

typedef struct zrpc_iobuf {
    zrpc_iobuf_blk_t *head;
    zrpc_iobuf_blk_t *tail;
    size_t total; /* 未消费字节数 */
} zrpc_iobuf_t;

void zrpc_iobuf_init(zrpc_iobuf_t *b);
/* 释放所有 block（含未消费部分）。调用后 iobuf 处于已 init 的空状态。 */
void zrpc_iobuf_reset(zrpc_iobuf_t *b);

size_t zrpc_iobuf_len(const zrpc_iobuf_t *b);
int zrpc_iobuf_empty(const zrpc_iobuf_t *b);

/*
 * 追加一段。owner 非空时对其 +1 引用，data 必须是该 ztk_buf 数据区内的一段。
 * 返回 0 成功，-1 失败（不改变原有内容）。
 */
int zrpc_iobuf_append_copy(zrpc_iobuf_t *b, ztk_poller *poller, const void *data, size_t len);
int zrpc_iobuf_append_buf(zrpc_iobuf_t *b, ztk_buf *owner, const void *data, size_t len);
int zrpc_iobuf_append_borrow(zrpc_iobuf_t *b, const void *data, size_t len);

/* 从逻辑偏移 off 起拷贝 n 字节到 dst（不消费）。返回实际拷贝字节数。 */
size_t zrpc_iobuf_peek(const zrpc_iobuf_t *b, size_t off, void *dst, size_t n);

/*
 * 把前缀导出为 iovec。最多 max_count 段、累计不超过 max_bytes（max_bytes=0 表示不限）。
 * 返回写入的 iov 数量。
 */
unsigned zrpc_iobuf_iovec(const zrpc_iobuf_t *b, ztk_socket_iov *iov, unsigned max_count,
                          size_t max_bytes);

/* 消费前缀 n 字节；整块消费完的 block 自动释放。n 超过当前长度时消费全部。 */
void zrpc_iobuf_consume(zrpc_iobuf_t *b, size_t n);

/*
 * 取前缀 n 字节的连续视图：若已连续（落在单个 block 内）直接返回内部指针，
 * 不拷贝；否则拷贝进 *scratch（必要时从 poller 池分配/扩容），返回其数据指针。
 * 返回值在下次 consume/reset 前有效。*scratch 可为 NULL（调用方不需要回收）。
 */
const uint8_t *zrpc_iobuf_view(zrpc_iobuf_t *b, size_t n, ztk_poller *poller, ztk_buf **scratch);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_IOBUF_H */
