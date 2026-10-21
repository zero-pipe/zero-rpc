/* payload codec 注册表（预留给应用层的序列化扩展点）。 */
#include "node_internal.h"

#include <stdlib.h>

/* ---- Codec 注册与查询 ---- */

int node_register_codec(node_t *node, const codec_t *codec)
{
    zri_codec_entry_t *e;
    if (!node || !codec) {
        return ERR_INVALID;
    }
    if (node_find_codec(node, codec->encoding)) {
        return ERR_STATE; /* encoding 已注册 */
    }
    e = (zri_codec_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        return ERR_NOMEM;
    }
    e->codec = *codec;
    e->next = node->codecs;
    node->codecs = e;
    return OK;
}

const codec_t *node_find_codec(const node_t *node, uint8_t encoding)
{
    zri_codec_entry_t *e;
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
