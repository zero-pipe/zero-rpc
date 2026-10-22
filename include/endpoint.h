#ifndef NB_ENDPOINT_H
#define NB_ENDPOINT_H

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 数据结构 ---- */
/* A transport-neutral endpoint. The scheme selects a transport provider. */
typedef struct nb_endpoint {
    char scheme[NB_SCHEME_MAX];
    char host[NB_ENDPOINT_MAX];
    uint16_t port;
    nb_transport_kind_t transport;
} nb_endpoint_t;

/* ---- Scheme 与传输映射 ---- */
/* Map a provider scheme to a built-in transport kind, if any. */
nb_transport_kind_t nb_transport_kind_from_scheme(const char *scheme);

/* ---- 解析与格式化 ---- */
/* Parse scheme://host:port. The input is not retained. */
int nb_endpoint_parse(const char *uri, nb_endpoint_t *out);

/* Format an endpoint URI into out. */
int nb_endpoint_format(const nb_endpoint_t *endpoint, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NB_ENDPOINT_H */
