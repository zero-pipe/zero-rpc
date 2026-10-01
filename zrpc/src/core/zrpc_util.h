#ifndef ZRPC_UTIL_H
#define ZRPC_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* 小工具：跨模块共享的内联辅助（避免多文件重复定义）。 */

static inline void zrpc_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static inline uint16_t zrpc_get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline void zrpc_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

static inline uint32_t zrpc_get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void zrpc_put_u64(uint8_t *p, uint64_t v) {
    zrpc_put_u32(p, (uint32_t)(v >> 32));
    zrpc_put_u32(p + 4, (uint32_t)v);
}

static inline uint64_t zrpc_get_u64(const uint8_t *p) {
    return ((uint64_t)zrpc_get_u32(p) << 32) | zrpc_get_u32(p + 4);
}

static inline void zrpc_copy_str(char *dst, size_t cap, const char *src, const char *def) {
    const char *s = (src && src[0]) ? src : def;
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (i + 1 < cap && s[i]) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
}

#endif /* ZRPC_UTIL_H */
