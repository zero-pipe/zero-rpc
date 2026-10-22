#ifndef UTIL_CRC32_H
#define UTIL_CRC32_H

#include <stddef.h>
#include <stdint.h>

/*
 * 增量 CRC32（IEEE 802.3，反射多项式 0xEDB88320）。首段 seed 传 0，后续段传上
 * 一次返回值即可拼接校验；用于 envelope 的完整性校验。
 */
uint32_t nb_crc32(const void *data, size_t len, uint32_t seed);

#endif /* UTIL_CRC32_H */
