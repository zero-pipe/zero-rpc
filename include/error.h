#ifndef NB_ERROR_H
#define NB_ERROR_H

/*
 * Public status codes returned by the network bridge framework SDK.
 *
 * Layout:
 *   0       success
 *   -1..-9  original v1 codes kept for source and wire compatibility
 *   -10..   additional framework conditions
 *   -1000   application-defined codes start here
 *
 * NB_ERROR_TABLE is the single source of truth for the code values, stable
 * names, human-readable descriptions and retry hints. src/util/error.c expands
 * the same table into the lookup helpers, so values and strings cannot drift.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * X(name, value, description, retryable)
 *
 * retryable is non-zero when retrying the same operation may succeed without
 * changing application state. Callers must still consider whether the request
 * is idempotent.
 */
#define NB_ERROR_TABLE(X) \
    /* Success and legacy values. Keep existing values stable. */ \
    X(NB_OK,                      0, "success",                                      0) \
    X(NB_ERR_INVALID,            -1, "invalid argument",                             0) \
    X(NB_ERR_NOMEM,              -2, "out of memory",                                0) \
    X(NB_ERR_NOTFOUND,           -3, "not found",                                    0) \
    X(NB_ERR_TIMEOUT,            -4, "operation timed out",                          0) \
    X(NB_ERR_IO,                 -5, "I/O error",                                    0) \
    X(NB_ERR_STATE,              -6, "invalid state",                                0) \
    X(NB_ERR_AGAIN,              -7, "resource temporarily unavailable",             1) \
    X(NB_ERR_TOOBIG,             -8, "message or value too large",                   0) \
    X(NB_ERR_CLOSED,             -9, "object or connection closed",                  0) \
    /* Extended framework conditions. */ \
    X(NB_ERR_ALREADY,           -10, "operation already started or entry exists",    0) \
    X(NB_ERR_NOT_SUPPORTED,     -11, "operation not supported",                      0) \
    X(NB_ERR_CANCELED,          -12, "operation canceled",                           0) \
    X(NB_ERR_OVERLOAD,          -13, "server overloaded; retry later",               1) \
    X(NB_ERR_BAD_MESSAGE,       -14, "malformed message or payload",                 0) \
    X(NB_ERR_PERMISSION,        -15, "permission denied",                            0) \
    X(NB_ERR_ADDR_IN_USE,       -16, "address already in use",                       0) \
    X(NB_ERR_CONN_REFUSED,      -17, "connection refused",                           0) \
    X(NB_ERR_CONN_RESET,        -18, "connection reset by peer",                     0) \
    X(NB_ERR_BUSY,              -19, "resource busy",                                1) \
    /* Application-defined codes: values at or below this base. */ \
    X(NB_ERR_USER_BASE,       -1000, "application-defined error range",              0)

enum {
#define NB_ERROR_ENUM_ENTRY(name, value, description, retryable) name = value,
    NB_ERROR_TABLE(NB_ERROR_ENUM_ENTRY)
#undef NB_ERROR_ENUM_ENTRY
};

/* Stable symbolic name, for example "NB_ERR_TIMEOUT". Never returns NULL. */
const char *nb_error_name(int status);

/* Human-readable description. Never returns NULL. */
const char *nb_strerror(int status);

/* Non-zero when the operation may be retried. */
int nb_error_is_retryable(int status);

#ifdef __cplusplus
}
#endif

#endif /* NB_ERROR_H */
