#include "util.h"

#include <stdio.h>

int nb_route_format(char *out, size_t cap, const char *service, const char *method)
{
    int n;

    if (!out || cap == 0 || !service || !method) {
        return NB_ERR_INVALID;
    }
    n = snprintf(out, cap, "%s.%s", service, method);
    if (n < 0 || (size_t)n >= cap) {
        return NB_ERR_INVALID;
    }
    return NB_OK;
}
