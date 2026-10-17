#ifndef ENDPOINT_H
#define ENDPOINT_H

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 数据结构 ---- */
/* A transport-neutral endpoint. The scheme selects a transport provider. */
typedef struct endpoint {
    char scheme[SCHEME_MAX];
    char host[ENDPOINT_MAX];
    uint16_t port;
    transport_kind_t transport;
} endpoint_t;

/* ---- Scheme 与传输映射 ---- */
/* Map a provider scheme to a built-in transport kind, if any. */
transport_kind_t transport_kind_from_scheme(const char *scheme);

/* ---- 解析与格式化 ---- */
/* Parse scheme://host:port. The input is not retained. */
int endpoint_parse(const char *uri, endpoint_t *out);

/* Format an endpoint URI into out. */
int endpoint_format(const endpoint_t *endpoint, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ENDPOINT_H */
