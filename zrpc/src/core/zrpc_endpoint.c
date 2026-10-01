#include <zrpc/zrpc_endpoint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_part(char *out, size_t cap, const char *begin, size_t len) {
    if (!out || cap == 0 || len == 0 || len >= cap) {
        return ZRPC_ERR_INVALID;
    }
    memcpy(out, begin, len);
    out[len] = '\0';
    return ZRPC_OK;
}

zrpc_transport_kind_t zrpc_transport_kind_from_scheme(const char *scheme) {
    if (!scheme) {
        return (zrpc_transport_kind_t)-1;
    }
    if (strcmp(scheme, "tcp") == 0 || strcmp(scheme, "TCP") == 0) {
        return ZRPC_TRANSPORT_TCP;
    }
    if (strcmp(scheme, "udp") == 0 || strcmp(scheme, "UDP") == 0) {
        return ZRPC_TRANSPORT_UDP;
    }
    return ZRPC_TRANSPORT_CUSTOM;
}

int zrpc_endpoint_parse(const char *uri, zrpc_endpoint_t *out) {
    const char *scheme_end;
    const char *authority;
    const char *port_sep;
    char port_text[8];
    char *port_end;
    unsigned long port;
    size_t host_len;

    if (!uri || !out) {
        return ZRPC_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    scheme_end = strstr(uri, "://");
    if (!scheme_end || scheme_end == uri) {
        return ZRPC_ERR_INVALID;
    }
    if (copy_part(out->scheme, sizeof(out->scheme), uri, (size_t)(scheme_end - uri)) != ZRPC_OK) {
        return ZRPC_ERR_INVALID;
    }
    authority = scheme_end + 3;
    if (!*authority) {
        return ZRPC_ERR_INVALID;
    }

    if (*authority == '[') {
        const char *close = strchr(authority, ']');
        if (!close || close[1] != ':') {
            return ZRPC_ERR_INVALID;
        }
        host_len = (size_t)(close - authority - 1);
        if (copy_part(out->host, sizeof(out->host), authority + 1, host_len) != ZRPC_OK) {
            return ZRPC_ERR_INVALID;
        }
        port_sep = close + 2;
    } else {
        port_sep = strrchr(authority, ':');
        if (!port_sep || port_sep == authority) {
            return ZRPC_ERR_INVALID;
        }
        host_len = (size_t)(port_sep - authority);
        if (copy_part(out->host, sizeof(out->host), authority, host_len) != ZRPC_OK) {
            return ZRPC_ERR_INVALID;
        }
    }
    if (*port_sep == ':') {
        port_sep++;
    }
    if (!*port_sep || strlen(port_sep) >= sizeof(port_text)) {
        return ZRPC_ERR_INVALID;
    }
    strcpy(port_text, port_sep);
    port = strtoul(port_text, &port_end, 10);
    if (*port_end != '\0' || port > 65535) {
        return ZRPC_ERR_INVALID;
    }
    out->port = (uint16_t)port;
    out->transport = zrpc_transport_kind_from_scheme(out->scheme);
    return ZRPC_OK;
}

int zrpc_endpoint_format(const zrpc_endpoint_t *endpoint, char *out, size_t cap) {
    int n;
    if (!endpoint || !out || cap == 0 || !endpoint->scheme[0] || !endpoint->host[0] ||
        !endpoint->port) {
        return ZRPC_ERR_INVALID;
    }
    if (strchr(endpoint->host, ':')) {
        n = snprintf(out, cap, "%s://[%s]:%u", endpoint->scheme, endpoint->host,
                     (unsigned)endpoint->port);
    } else {
        n = snprintf(out, cap, "%s://%s:%u", endpoint->scheme, endpoint->host,
                     (unsigned)endpoint->port);
    }
    return n >= 0 && (size_t)n < cap ? ZRPC_OK : ZRPC_ERR_TOOBIG;
}
