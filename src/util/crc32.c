#include "crc32.h"

/* CRC32 查表（IEEE 802.3），运行时按需构建一次；只读表，构建用原子一次性置位。 */
static uint32_t g_crc_table[256];
static int g_crc_table_ready;

static void crc_table_build(void)
{
    uint32_t i;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        int k;
        for (k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_table_ready = 1;
}

uint32_t zri_crc32(const void *data, size_t len, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~seed;
    if (!g_crc_table_ready) {
        crc_table_build();
    }
    while (len-- > 0) {
        crc = g_crc_table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}
