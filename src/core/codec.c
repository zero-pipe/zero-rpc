/* payload codec 注册表（预留给应用层的序列化扩展点）。 */
#include "node_internal.h"

#include <stdlib.h>

/* ---- Codec 注册与查询 ---- */

static const codec_t *find_codec_unlocked(const node_t *node, uint8_t encoding)
{
    zri_codec_entry_t *e;
    for (e = node->codecs; e; e = e->next) {
        if (e->codec.encoding == encoding) {
            return &e->codec;
        }
    }
    return NULL;
}

int node_register_codec(node_t *node, const codec_t *codec)
{
    zri_codec_entry_t *e;
    if (!node || !codec) {
        return ERR_INVALID;
    }
    if (node->codecs_lock) {
        ntk_mutex_lock(node->codecs_lock);
    }
    if (find_codec_unlocked(node, codec->encoding)) {
        if (node->codecs_lock) {
            ntk_mutex_unlock(node->codecs_lock);
        }
        return ERR_STATE; /* encoding 已注册 */
    }
    e = (zri_codec_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        if (node->codecs_lock) {
            ntk_mutex_unlock(node->codecs_lock);
        }
        return ERR_NOMEM;
    }
    e->codec = *codec;
    e->next = node->codecs;
    node->codecs = e;
    if (node->codecs_lock) {
        ntk_mutex_unlock(node->codecs_lock);
    }
    return OK;
}

const codec_t *node_find_codec(const node_t *node, uint8_t encoding)
{
    const codec_t *found;
    if (!node) {
        return NULL;
    }
    if (node->codecs_lock) {
        ntk_mutex_lock((ntk_mutex *)node->codecs_lock);
    }
    found = find_codec_unlocked(node, encoding);
    if (node->codecs_lock) {
        ntk_mutex_unlock((ntk_mutex *)node->codecs_lock);
    }
    return found;
}
