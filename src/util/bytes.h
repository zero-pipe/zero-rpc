#ifndef UTIL_BYTES_H
#define UTIL_BYTES_H

/* 大端（network order）字节序读写：跨模块共享的内联辅助。 */

#include <stdint.h>

static inline void nb_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static inline uint16_t nb_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline void nb_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

static inline uint32_t nb_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void nb_put_u64(uint8_t *p, uint64_t v)
{
    nb_put_u32(p, (uint32_t)(v >> 32));
    nb_put_u32(p + 4, (uint32_t)v);
}

static inline uint64_t nb_get_u64(const uint8_t *p)
{
    return ((uint64_t)nb_get_u32(p) << 32) | nb_get_u32(p + 4);
}

#endif /* UTIL_BYTES_H */
