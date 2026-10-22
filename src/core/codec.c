/* payload codec 注册表（预留给应用层的序列化扩展点）。 */
#include "node_internal.h"

#include <stdlib.h>

/* ---- Codec 注册与查询 ---- */

static const nb_codec_t *nb_find_codec_unlocked(const nb_node_t *node, uint8_t encoding)
{
    nb_codec_entry_t *e;
    for (e = node->codecs; e; e = e->next) {
        if (e->codec.encoding == encoding) {
            return &e->codec;
        }
    }
    return NULL;
}

int nb_node_register_codec(nb_node_t *node, const nb_codec_t *codec)
{
    nb_codec_entry_t *e;
    if (!node || !codec) {
        return NB_ERR_INVALID;
    }
    if (node->codecs_lock) {
        ntk_mutex_lock(node->codecs_lock);
    }
    if (nb_find_codec_unlocked(node, codec->encoding)) {
        if (node->codecs_lock) {
            ntk_mutex_unlock(node->codecs_lock);
        }
        return NB_ERR_STATE; /* encoding 已注册 */
    }
    e = (nb_codec_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        if (node->codecs_lock) {
            ntk_mutex_unlock(node->codecs_lock);
        }
        return NB_ERR_NOMEM;
    }
    e->codec = *codec;
    e->next = node->codecs;
    node->codecs = e;
    if (node->codecs_lock) {
        ntk_mutex_unlock(node->codecs_lock);
    }
    return NB_OK;
}

const nb_codec_t *nb_node_find_codec(const nb_node_t *node, uint8_t encoding)
{
    const nb_codec_t *found;
    if (!node) {
        return NULL;
    }
    if (node->codecs_lock) {
        ntk_mutex_lock((ntk_mutex *)node->codecs_lock);
    }
    found = nb_find_codec_unlocked(node, encoding);
    if (node->codecs_lock) {
        ntk_mutex_unlock((ntk_mutex *)node->codecs_lock);
    }
    return found;
}
