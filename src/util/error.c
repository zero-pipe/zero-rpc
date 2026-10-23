/*
 * Status-name and retryability lookup generated from NB_ERROR_TABLE.
 */
#include <error.h>

#include <stddef.h>

typedef struct nb_error_entry {
    int status;
    const char *name;
    const char *description;
    int retryable;
} nb_error_entry_t;

static const nb_error_entry_t nb_error_entries[] = {
#define NB_ERROR_TABLE_ENTRY(name, value, description, retryable) \
    { value, #name, description, retryable },
    NB_ERROR_TABLE(NB_ERROR_TABLE_ENTRY)
#undef NB_ERROR_TABLE_ENTRY
};

#define NB_ERROR_ENTRY_COUNT \
    (sizeof(nb_error_entries) / sizeof(nb_error_entries[0]))

static const nb_error_entry_t *nb_error_find(int status)
{
    size_t i;

    for (i = 0; i < NB_ERROR_ENTRY_COUNT; ++i) {
        if (nb_error_entries[i].status == status) {
            return &nb_error_entries[i];
        }
    }
    return NULL;
}

const char *nb_error_name(int status)
{
    const nb_error_entry_t *entry = nb_error_find(status);

    return entry ? entry->name : "NB_ERR_UNKNOWN";
}

const char *nb_strerror(int status)
{
    const nb_error_entry_t *entry = nb_error_find(status);

    return entry ? entry->description : "unknown error";
}

int nb_error_is_retryable(int status)
{
    const nb_error_entry_t *entry = nb_error_find(status);

    return entry ? entry->retryable : 0;
}
