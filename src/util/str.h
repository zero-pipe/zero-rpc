#ifndef UTIL_STRING_H
#define UTIL_STRING_H

#include <stddef.h>

/* 安全字符串拷贝：src 为空时用 def，始终 NUL 结尾。 */
static inline void nb_copy_str(char *dst, size_t cap, const char *src, const char *def)
{
    const char *s = (src && src[0]) ? src : def;
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    if (!s) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < cap && s[i]) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
}

#endif /* UTIL_STRING_H */
