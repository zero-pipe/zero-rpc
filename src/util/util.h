#ifndef UTIL_H
#define UTIL_H

/*
 * 小工具伞头：跨模块共享的内联辅助。
 *
 * 具体能力拆到独立头，按需 include 更清晰：
 *   bytes.h   大端字节序读写
 *   crc32.h   CRC32 校验
 *   str.h     安全字符串拷贝
 * 本伞头保留给"用得多、不想逐个 include"的场景，并声明跨模块的少量函数。
 */

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "crc32.h"
#include "str.h"

/* 单调毫秒时钟（实现于 node.c）。 */
uint64_t nb_now_ms(void);

/* 格式化 "service.method"；超长/参数非法返回 NB_ERR_INVALID。 */
int nb_route_format(char *out, size_t cap, const char *service, const char *method);

#endif /* UTIL_H */
