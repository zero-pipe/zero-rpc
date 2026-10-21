/* 轻量 metadata：固定容量、字符串 key + 二进制 value，wire 为 TLV。 */
#include <types.h>

#include <string.h>

static metadata_entry_t *metadata_find(metadata_t *metadata, const char *key)
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

void metadata_init(metadata_t *metadata)
{
    if (metadata) {
        metadata->count = 0;
    }
}

int metadata_set(metadata_t *metadata, const char *key, const void *value, size_t len)
{
    metadata_entry_t *entry;
    size_t key_len;

    if (!metadata || !key) {
        return ERR_INVALID;
    }
    key_len = strlen(key);
    if (key_len == 0 || key_len >= METADATA_KEY_MAX || len > METADATA_VALUE_MAX ||
        (len > 0 && !value)) {
        return ERR_INVALID;
    }
    entry = metadata_find(metadata, key);
    if (!entry) {
        if (metadata->count >= METADATA_MAX_ENTRIES) {
            return ERR_TOOBIG;
        }
        entry = &metadata->entries[metadata->count++];
        memcpy(entry->key, key, key_len + 1);
    }
    if (len > 0) {
        memcpy(entry->value, value, len);
    }
    entry->value_len = (uint16_t)len;
    return OK;
}

int metadata_set_string(metadata_t *metadata, const char *key, const char *value)
{
    if (!value) {
        return ERR_INVALID;
    }
    /* 连同结尾 '\0' 一起存，metadata_get_string 可直接返回。 */
    return metadata_set(metadata, key, value, strlen(value) + 1);
}

const void *metadata_get(const metadata_t *metadata, const char *key, size_t *len)
{
    metadata_entry_t *entry = metadata_find((metadata_t *)metadata, key);
    if (!entry) {
        return NULL;
    }
    if (len) {
        *len = entry->value_len;
    }
    return entry->value;
}

const char *metadata_get_string(const metadata_t *metadata, const char *key)
{
    metadata_entry_t *entry = metadata_find((metadata_t *)metadata, key);
    if (!entry || entry->value_len == 0) {
        return NULL;
    }
    return (const char *)entry->value;
}

int metadata_remove(metadata_t *metadata, const char *key)
{
    uint16_t i;
    if (!metadata || !key) {
        return ERR_INVALID;
    }
    for (i = 0; i < metadata->count; i++) {
        if (strcmp(metadata->entries[i].key, key) == 0) {
            if (i + 1 < metadata->count) {
                memmove(&metadata->entries[i],
                        &metadata->entries[i + 1],
                        (size_t)(metadata->count - i - 1) * sizeof(metadata->entries[0]));
            }
            metadata->count--;
            return OK;
        }
    }
    return ERR_NOTFOUND;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int metadata_encode(const metadata_t *metadata, void *buf, size_t cap, size_t *out_len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    uint16_t i;

    if (out_len) {
        *out_len = 0;
    }
    if (!metadata || !buf) {
        return metadata && metadata->count == 0 ? OK : ERR_INVALID;
    }
    for (i = 0; i < metadata->count; i++) {
        const metadata_entry_t *entry = &metadata->entries[i];
        size_t key_len = strlen(entry->key);
        size_t need = 4 + key_len + entry->value_len;
        if (off + need > cap || need > METADATA_WIRE_MAX) {
            return ERR_TOOBIG;
        }
        put_u16(p + off, (uint16_t)key_len);
        put_u16(p + off + 2, entry->value_len);
        memcpy(p + off + 4, entry->key, key_len);
        memcpy(p + off + 4 + key_len, entry->value, entry->value_len);
        off += need;
    }
    if (out_len) {
        *out_len = off;
    }
    return OK;
}

int metadata_decode(metadata_t *metadata, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    if (!metadata || (len > 0 && !buf)) {
        return ERR_INVALID;
    }
    metadata_init(metadata);
    while (off < len) {
        uint16_t key_len;
        uint16_t value_len;
        char key[METADATA_KEY_MAX];
        int rc;
        if (len - off < 4) {
            return ERR_INVALID;
        }
        key_len = get_u16(p + off);
        value_len = get_u16(p + off + 2);
        off += 4;
        if (key_len == 0 || key_len >= METADATA_KEY_MAX ||
            len - off < (size_t)key_len + value_len) {
            return ERR_INVALID;
        }
        memcpy(key, p + off, key_len);
        key[key_len] = '\0';
        off += key_len;
        rc = metadata_set(metadata, key, p + off, value_len);
        if (rc != OK) {
            return rc;
        }
        off += value_len;
    }
    return OK;
}
