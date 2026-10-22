/* 轻量 metadata：固定容量、字符串 key + 二进制 value，wire 为 TLV。 */
#include <types.h>

#include <string.h>

static nb_metadata_entry_t *nb_metadata_find(nb_metadata_t *metadata, const char *key)
{
    uint16_t i;
    if (!metadata || !key) {
        return NULL;
    }
    for (i = 0; i < metadata->count; i++) {
        if (strcmp(metadata->entries[i].key, key) == 0) {
            return &metadata->entries[i];
        }
    }
    return NULL;
}

void nb_metadata_init(nb_metadata_t *metadata)
{
    if (metadata) {
        metadata->count = 0;
    }
}

int nb_metadata_set(nb_metadata_t *metadata, const char *key, const void *value, size_t len)
{
    nb_metadata_entry_t *entry;
    size_t key_len;

    if (!metadata || !key) {
        return NB_ERR_INVALID;
    }
    key_len = strlen(key);
    if (key_len == 0 || key_len >= NB_METADATA_KEY_MAX || len > NB_METADATA_VALUE_MAX ||
        (len > 0 && !value)) {
        return NB_ERR_INVALID;
    }
    entry = nb_metadata_find(metadata, key);
    if (!entry) {
        if (metadata->count >= NB_METADATA_MAX_ENTRIES) {
            return NB_ERR_TOOBIG;
        }
        entry = &metadata->entries[metadata->count++];
        memcpy(entry->key, key, key_len + 1);
    }
    if (len > 0) {
        memcpy(entry->value, value, len);
    }
    entry->value_len = (uint16_t)len;
    return NB_OK;
}

int nb_metadata_set_string(nb_metadata_t *metadata, const char *key, const char *value)
{
    if (!value) {
        return NB_ERR_INVALID;
    }
    /* 连同结尾 '\0' 一起存，nb_metadata_get_string 可直接返回。 */
    return nb_metadata_set(metadata, key, value, strlen(value) + 1);
}

const void *nb_metadata_get(const nb_metadata_t *metadata, const char *key, size_t *len)
{
    nb_metadata_entry_t *entry = nb_metadata_find((nb_metadata_t *)metadata, key);
    if (!entry) {
        return NULL;
    }
    if (len) {
        *len = entry->value_len;
    }
    return entry->value;
}

const char *nb_metadata_get_string(const nb_metadata_t *metadata, const char *key)
{
    nb_metadata_entry_t *entry = nb_metadata_find((nb_metadata_t *)metadata, key);
    if (!entry || entry->value_len == 0) {
        return NULL;
    }
    return (const char *)entry->value;
}

int nb_metadata_remove(nb_metadata_t *metadata, const char *key)
{
    uint16_t i;
    if (!metadata || !key) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < metadata->count; i++) {
        if (strcmp(metadata->entries[i].key, key) == 0) {
            if (i + 1 < metadata->count) {
                memmove(&metadata->entries[i],
                        &metadata->entries[i + 1],
                        (size_t)(metadata->count - i - 1) * sizeof(metadata->entries[0]));
            }
            metadata->count--;
            return NB_OK;
        }
    }
    return NB_ERR_NOTFOUND;
}

static void nb_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t nb_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int nb_metadata_encode(const nb_metadata_t *metadata, void *buf, size_t cap, size_t *out_len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    uint16_t i;

    if (out_len) {
        *out_len = 0;
    }
    if (!metadata || !buf) {
        return metadata && metadata->count == 0 ? NB_OK : NB_ERR_INVALID;
    }
    for (i = 0; i < metadata->count; i++) {
        const nb_metadata_entry_t *entry = &metadata->entries[i];
        size_t key_len = strlen(entry->key);
        size_t need = 4 + key_len + entry->value_len;
        if (off + need > cap || need > NB_METADATA_WIRE_MAX) {
            return NB_ERR_TOOBIG;
        }
        nb_put_u16(p + off, (uint16_t)key_len);
        nb_put_u16(p + off + 2, entry->value_len);
        memcpy(p + off + 4, entry->key, key_len);
        memcpy(p + off + 4 + key_len, entry->value, entry->value_len);
        off += need;
    }
    if (out_len) {
        *out_len = off;
    }
    return NB_OK;
}

int nb_metadata_decode(nb_metadata_t *metadata, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    if (!metadata || (len > 0 && !buf)) {
        return NB_ERR_INVALID;
    }
    nb_metadata_init(metadata);
    while (off < len) {
        uint16_t key_len;
        uint16_t value_len;
        char key[NB_METADATA_KEY_MAX];
        int rc;
        if (len - off < 4) {
            return NB_ERR_INVALID;
        }
        key_len = nb_get_u16(p + off);
        value_len = nb_get_u16(p + off + 2);
        off += 4;
        if (key_len == 0 || key_len >= NB_METADATA_KEY_MAX ||
            len - off < (size_t)key_len + value_len) {
            return NB_ERR_INVALID;
        }
        memcpy(key, p + off, key_len);
        key[key_len] = '\0';
        off += key_len;
        rc = nb_metadata_set(metadata, key, p + off, value_len);
        if (rc != NB_OK) {
            return rc;
        }
        off += value_len;
    }
    return NB_OK;
}
