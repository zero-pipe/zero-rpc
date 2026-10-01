/* payload codec 注册表（预留给应用层的序列化扩展点）。 */
#include "zrpc_internal.h"

#include <stdlib.h>

int zrpc_node_register_codec(zrpc_node_t *node, const zrpc_codec_t *codec) {
    zrpc_codec_entry_t *e;
    if (!node || !codec) {
        return ZRPC_ERR_INVALID;
    }
    if (zrpc_node_find_codec(node, codec->encoding)) {
        return ZRPC_ERR_STATE; /* encoding 已注册 */
    }
    e = (zrpc_codec_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        return ZRPC_ERR_NOMEM;
    }
    e->codec = *codec;
    e->next = node->codecs;
    node->codecs = e;
    return ZRPC_OK;
}

const zrpc_codec_t *zrpc_node_find_codec(const zrpc_node_t *node, uint8_t encoding) {
    zrpc_codec_entry_t *e;
    if (!node) {
        return NULL;
    }
    for (e = node->codecs; e; e = e->next) {
        if (e->codec.encoding == encoding) {
            return &e->codec;
        }
    }
    return NULL;
}
