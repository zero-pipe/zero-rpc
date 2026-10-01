#ifndef ZRPC_CODEC_H
#define ZRPC_CODEC_H

/*
 * payload 编解码扩展点（预留给应用层）。
 *
 * 框架本身不解释 payload；用户可注册一个 zrpc_codec_t，按 encoding 标识把
 * 业务对象与字节流互转。协议层只携带 encoding 标签，序列化格式由应用决定，
 * 后续可平滑接入 JSON / IDL / protobuf 等。
 *
 * 典型用法：
 *   - 发送前：codec->encode(ctx, obj, &bytes, &len) 得到 payload；
 *   - 收到后：codec->decode(ctx, payload.data, payload.len, &obj) 还原对象；
 *   - 释放：codec->release(ctx, obj)。
 */

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_codec {
    const char *name;
    uint8_t encoding; /* 与 zrpc_payload_t.encoding 对应 */

    /* 业务对象 -> 字节流。 *out_data 由实现分配，调用方用 release 释放。 */
    int (*encode)(void *ctx, const void *obj, void **out_data, size_t *out_len);
    /* 字节流 -> 业务对象。 *out_obj 由实现分配，调用方用 release 释放。 */
    int (*decode)(void *ctx, const void *data, size_t len, void **out_obj);
    /* 释放 encode/decode 产生的对象。 */
    void (*release)(void *ctx, void *obj);

    void *ctx;
} zrpc_codec_t;

/* 在节点上注册/查找 codec。encoding 需唯一；查找失败返回 NULL。 */
int zrpc_node_register_codec(zrpc_node_t *node, const zrpc_codec_t *codec);
const zrpc_codec_t *zrpc_node_find_codec(const zrpc_node_t *node, uint8_t encoding);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_CODEC_H */
