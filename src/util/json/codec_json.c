#include "codec_json.h"

#include "cJSON.h"
#include "codec_json_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void* nb_field_ptr(void* obj, size_t offset) {
    return (char*)obj + offset;
}

static const void* nb_field_cptr(const void* obj, size_t offset) {
    return (const char*)obj + offset;
}

static int nb_model_obj_size(const nb_codec_json_model_t* model, size_t n, size_t* size) {
    size_t i;

    if (!model || !size || n == 0) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    for (i = 0; i < n; i++) {
        if (model[i].type == NB_CODEC_JSON_OBJ) {
            if (model[i].param.obj_size == 0) {
                return NB_CODEC_INVALID_ARGUMENT;
            }
            *size = model[i].param.obj_size;
            return NB_CODEC_OK;
        }
    }
    return NB_CODEC_INVALID_ARGUMENT;
}

static int nb_model_elem_size(const nb_codec_json_model_t* field, size_t* size) {
    nb_codec_json_type_t type;

    if (field->param.array.elem_size != 0) {
        *size = field->param.array.elem_size;
        return NB_CODEC_OK;
    }
    type = field->param.array.elem_type;
    switch (type) {
    case NB_CODEC_JSON_BOOL:
    case NB_CODEC_JSON_I8:
    case NB_CODEC_JSON_U8:
        *size = 1;
        return NB_CODEC_OK;
    case NB_CODEC_JSON_I16:
    case NB_CODEC_JSON_U16:
        *size = 2;
        return NB_CODEC_OK;
    case NB_CODEC_JSON_I32:
    case NB_CODEC_JSON_U32:
    case NB_CODEC_JSON_F32:
        *size = 4;
        return NB_CODEC_OK;
    case NB_CODEC_JSON_I64:
    case NB_CODEC_JSON_U64:
    case NB_CODEC_JSON_F64:
        *size = 8;
        return NB_CODEC_OK;
    case NB_CODEC_JSON_STRING:
    case NB_CODEC_JSON_RAW:
    case NB_CODEC_JSON_STRUCT:
        *size = sizeof(void*);
        return NB_CODEC_OK;
    case NB_CODEC_JSON_INLINE:
        return nb_model_obj_size(field->param.array.model, field->param.array.model_count, size);
    default:
        return NB_CODEC_INVALID_ARGUMENT;
    }
}

static int nb_dup_string(const char* src, size_t n, char** out) {
    char* copy = (char*)malloc(n + 1u);
    if (!copy) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    if (n != 0) {
        memcpy(copy, src, n);
    }
    copy[n] = '\0';
    *out = copy;
    return NB_CODEC_OK;
}

static int nb_json_as_i64(const cJSON* v, int64_t* out) {
    double d;
    int64_t i;

    if (!cJSON_IsNumber(v)) {
        return NB_CODEC_TYPE_MISMATCH;
    }
    d = v->valuedouble;
    if (d != d || d < (double)INT64_MIN || d > (double)INT64_MAX) {
        return NB_CODEC_LIMIT_EXCEEDED;
    }
    i = (int64_t)d;
    if ((double)i != d) {
        return NB_CODEC_TYPE_MISMATCH;
    }
    *out = i;
    return NB_CODEC_OK;
}

static int nb_json_as_u64(const cJSON* v, uint64_t* out) {
    double d;
    uint64_t u;

    if (!cJSON_IsNumber(v)) {
        return NB_CODEC_TYPE_MISMATCH;
    }
    d = v->valuedouble;
    if (d != d || d < 0.0 || d > (double)UINT64_MAX) {
        return NB_CODEC_LIMIT_EXCEEDED;
    }
    u = (uint64_t)d;
    if ((double)u != d) {
        return NB_CODEC_TYPE_MISMATCH;
    }
    *out = u;
    return NB_CODEC_OK;
}

static int nb_fill_object(const cJSON* json, void* obj, const nb_codec_json_model_t* model,
                       size_t model_count);
static int nb_encode_object(const void* obj, const nb_codec_json_model_t* model, size_t model_count,
                         cJSON** out);
static void nb_clear_object(void* obj, const nb_codec_json_model_t* model, size_t model_count,
                         int free_root);
static void nb_clear_array_items(void* base, size_t count, const nb_codec_json_model_t* field);
static void nb_clear_map(nb_codec_json_map_t* map, const nb_codec_json_model_t* field);

static int nb_decode_elem(const cJSON* v, nb_codec_json_type_t type, void* dest,
                       const nb_codec_json_model_t* sub, size_t sub_n) {
    int status;
    size_t size;

    if (!v || cJSON_IsNull(v)) {
        if (type == NB_CODEC_JSON_STRING || type == NB_CODEC_JSON_RAW ||
            type == NB_CODEC_JSON_STRUCT) {
            *(void**)dest = NULL;
            return NB_CODEC_OK;
        }
        if (type == NB_CODEC_JSON_INLINE) {
            status = nb_model_obj_size(sub, sub_n, &size);
            if (status != NB_CODEC_OK) {
                return status;
            }
            memset(dest, 0, size);
            return NB_CODEC_OK;
        }
        switch (type) {
        case NB_CODEC_JSON_BOOL:
        case NB_CODEC_JSON_I8:
        case NB_CODEC_JSON_U8:
            memset(dest, 0, 1);
            return NB_CODEC_OK;
        case NB_CODEC_JSON_I16:
        case NB_CODEC_JSON_U16:
            memset(dest, 0, 2);
            return NB_CODEC_OK;
        case NB_CODEC_JSON_I32:
        case NB_CODEC_JSON_U32:
        case NB_CODEC_JSON_F32:
            memset(dest, 0, 4);
            return NB_CODEC_OK;
        case NB_CODEC_JSON_I64:
        case NB_CODEC_JSON_U64:
        case NB_CODEC_JSON_F64:
            memset(dest, 0, 8);
            return NB_CODEC_OK;
        default:
            return NB_CODEC_TYPE_MISMATCH;
        }
    }

    switch (type) {
    case NB_CODEC_JSON_BOOL: {
        uint8_t b;
        if (!cJSON_IsBool(v)) {
            return NB_CODEC_TYPE_MISMATCH;
        }
        b = cJSON_IsTrue(v) ? 1u : 0u;
        memcpy(dest, &b, 1);
        return NB_CODEC_OK;
    }
    case NB_CODEC_JSON_I8:
    case NB_CODEC_JSON_I16:
    case NB_CODEC_JSON_I32:
    case NB_CODEC_JSON_I64: {
        int64_t i;
        status = nb_json_as_i64(v, &i);
        if (status != NB_CODEC_OK) {
            return status;
        }
        if (type == NB_CODEC_JSON_I8) {
            int8_t x;
            if (i < INT8_MIN || i > INT8_MAX) {
                return NB_CODEC_LIMIT_EXCEEDED;
            }
            x = (int8_t)i;
            memcpy(dest, &x, sizeof(x));
        } else if (type == NB_CODEC_JSON_I16) {
            int16_t x;
            if (i < INT16_MIN || i > INT16_MAX) {
                return NB_CODEC_LIMIT_EXCEEDED;
            }
            x = (int16_t)i;
            memcpy(dest, &x, sizeof(x));
        } else if (type == NB_CODEC_JSON_I32) {
            int32_t x;
            if (i < INT32_MIN || i > INT32_MAX) {
                return NB_CODEC_LIMIT_EXCEEDED;
            }
            x = (int32_t)i;
            memcpy(dest, &x, sizeof(x));
        } else {
            memcpy(dest, &i, sizeof(i));
        }
        return NB_CODEC_OK;
    }
    case NB_CODEC_JSON_U8:
    case NB_CODEC_JSON_U16:
    case NB_CODEC_JSON_U32:
    case NB_CODEC_JSON_U64: {
        uint64_t u;
        status = nb_json_as_u64(v, &u);
        if (status != NB_CODEC_OK) {
            return status;
        }
        if (type == NB_CODEC_JSON_U8) {
            uint8_t x;
            if (u > UINT8_MAX) {
                return NB_CODEC_LIMIT_EXCEEDED;
            }
            x = (uint8_t)u;
            memcpy(dest, &x, sizeof(x));
        } else if (type == NB_CODEC_JSON_U16) {
            uint16_t x;
            if (u > UINT16_MAX) {
                return NB_CODEC_LIMIT_EXCEEDED;
            }
            x = (uint16_t)u;
            memcpy(dest, &x, sizeof(x));
        } else if (type == NB_CODEC_JSON_U32) {
            uint32_t x;
            if (u > UINT32_MAX) {
                return NB_CODEC_LIMIT_EXCEEDED;
            }
            x = (uint32_t)u;
            memcpy(dest, &x, sizeof(x));
        } else {
            memcpy(dest, &u, sizeof(u));
        }
        return NB_CODEC_OK;
    }
    case NB_CODEC_JSON_F32: {
        float f;
        if (!cJSON_IsNumber(v)) {
            return NB_CODEC_TYPE_MISMATCH;
        }
        f = (float)v->valuedouble;
        memcpy(dest, &f, sizeof(f));
        return NB_CODEC_OK;
    }
    case NB_CODEC_JSON_F64: {
        double d;
        if (!cJSON_IsNumber(v)) {
            return NB_CODEC_TYPE_MISMATCH;
        }
        d = v->valuedouble;
        memcpy(dest, &d, sizeof(d));
        return NB_CODEC_OK;
    }
    case NB_CODEC_JSON_STRING: {
        const char* s;
        if (!cJSON_IsString(v) || !v->valuestring) {
            return NB_CODEC_TYPE_MISMATCH;
        }
        s = v->valuestring;
        return nb_dup_string(s, strlen(s), (char**)dest);
    }
    case NB_CODEC_JSON_RAW: {
        char* printed;
        if (cJSON_IsString(v) && v->valuestring) {
            return nb_dup_string(v->valuestring, strlen(v->valuestring), (char**)dest);
        }
        printed = cJSON_PrintUnformatted(v);
        if (!printed) {
            return NB_CODEC_OUT_OF_MEMORY;
        }
        status = nb_dup_string(printed, strlen(printed), (char**)dest);
        cJSON_free(printed);
        return status;
    }
    case NB_CODEC_JSON_STRUCT: {
        void* nested;
        if (!cJSON_IsObject(v)) {
            return NB_CODEC_TYPE_MISMATCH;
        }
        status = nb_model_obj_size(sub, sub_n, &size);
        if (status != NB_CODEC_OK) {
            return status;
        }
        nested = calloc(1, size);
        if (!nested) {
            return NB_CODEC_OUT_OF_MEMORY;
        }
        status = nb_fill_object(v, nested, sub, sub_n);
        if (status != NB_CODEC_OK) {
            nb_clear_object(nested, sub, sub_n, 1);
            return status;
        }
        *(void**)dest = nested;
        return NB_CODEC_OK;
    }
    case NB_CODEC_JSON_INLINE:
        if (!cJSON_IsObject(v)) {
            return NB_CODEC_TYPE_MISMATCH;
        }
        return nb_fill_object(v, dest, sub, sub_n);
    default:
        return NB_CODEC_INVALID_ARGUMENT;
    }
}

static int nb_decode_array(const cJSON* v, void* base, const nb_codec_json_model_t* field) {
    size_t i;
    size_t elem_size;
    size_t count;
    int n;
    int status;

    if (!cJSON_IsArray(v)) {
        return NB_CODEC_TYPE_MISMATCH;
    }
    status = nb_model_elem_size(field, &elem_size);
    if (status != NB_CODEC_OK) {
        return status;
    }
    n = cJSON_GetArraySize(v);
    if (n < 0) {
        return NB_CODEC_MALFORMED;
    }
    count = field->param.array.elem_count;
    if ((size_t)n > count) {
        return NB_CODEC_LIMIT_EXCEEDED;
    }
    for (i = 0; i < (size_t)n; i++) {
        status = nb_decode_elem(cJSON_GetArrayItem(v, (int)i), field->param.array.elem_type,
                             (char*)base + i * elem_size, field->param.array.model,
                             field->param.array.model_count);
        if (status != NB_CODEC_OK) {
            return status;
        }
    }
    return NB_CODEC_OK;
}

static int nb_decode_vec(const cJSON* v, nb_codec_json_vec_t* vec, const nb_codec_json_model_t* field) {
    size_t i;
    size_t elem_size;
    size_t n;
    int raw_n;
    void* items;
    int status;

    if (!cJSON_IsArray(v)) {
        return NB_CODEC_TYPE_MISMATCH;
    }
    status = nb_model_elem_size(field, &elem_size);
    if (status != NB_CODEC_OK) {
        return status;
    }
    raw_n = cJSON_GetArraySize(v);
    if (raw_n < 0) {
        return NB_CODEC_MALFORMED;
    }
    n = (size_t)raw_n;
    if (n == 0) {
        vec->items = NULL;
        vec->count = 0;
        return NB_CODEC_OK;
    }
    if (elem_size != 0 && n > SIZE_MAX / elem_size) {
        return NB_CODEC_LIMIT_EXCEEDED;
    }
    items = calloc(n, elem_size);
    if (!items) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    for (i = 0; i < n; i++) {
        status = nb_decode_elem(cJSON_GetArrayItem(v, (int)i), field->param.array.elem_type,
                             (char*)items + i * elem_size, field->param.array.model,
                             field->param.array.model_count);
        if (status != NB_CODEC_OK) {
            nb_clear_array_items(items, i, field);
            free(items);
            return status;
        }
    }
    vec->items = items;
    vec->count = n;
    return NB_CODEC_OK;
}

static int nb_decode_map(const cJSON* v, nb_codec_json_map_t* map, const nb_codec_json_model_t* field) {
    size_t size;
    size_t index = 0;
    nb_codec_json_map_entry_t* entries;
    const cJSON* item;
    int n;
    int status;

    if (!cJSON_IsObject(v)) {
        return NB_CODEC_TYPE_MISMATCH;
    }
    status = nb_model_obj_size(field->param.sub.model, field->param.sub.count, &size);
    if (status != NB_CODEC_OK) {
        return status;
    }
    n = cJSON_GetArraySize(v);
    if (n < 0) {
        return NB_CODEC_MALFORMED;
    }
    map->count = (size_t)n;
    map->entries = NULL;
    if (map->count == 0) {
        return NB_CODEC_OK;
    }
    entries = (nb_codec_json_map_entry_t*)calloc(map->count, sizeof(*entries));
    if (!entries) {
        map->count = 0;
        return NB_CODEC_OUT_OF_MEMORY;
    }
    map->entries = entries;
    cJSON_ArrayForEach(item, v) {
        void* value;

        if (!item->string) {
            nb_clear_map(map, field);
            return NB_CODEC_MALFORMED;
        }
        status = nb_dup_string(item->string, strlen(item->string), &entries[index].key);
        if (status != NB_CODEC_OK) {
            nb_clear_map(map, field);
            return status;
        }
        value = calloc(1, size);
        if (!value) {
            nb_clear_map(map, field);
            return NB_CODEC_OUT_OF_MEMORY;
        }
        entries[index].value = value;
        status = nb_fill_object(item, value, field->param.sub.model, field->param.sub.count);
        if (status != NB_CODEC_OK) {
            nb_clear_map(map, field);
            return status;
        }
        index++;
    }
    return NB_CODEC_OK;
}

static int nb_fill_object(const cJSON* json, void* obj, const nb_codec_json_model_t* model,
                       size_t model_count) {
    size_t i;
    int status;

    if (!json || !cJSON_IsObject(json) || !obj || !model) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    for (i = 0; i < model_count; i++) {
        const nb_codec_json_model_t* field = &model[i];
        const cJSON* item;
        void* dest;

        if (field->type == NB_CODEC_JSON_OBJ) {
            continue;
        }
        if (!field->key) {
            return NB_CODEC_INVALID_ARGUMENT;
        }
        item = cJSON_GetObjectItemCaseSensitive(json, field->key);
        if (!item) {
            continue;
        }
        dest = nb_field_ptr(obj, field->offset);
        if (field->presence_offset != 0) {
            *(uint8_t*)nb_field_ptr(obj, field->presence_offset) = 1u;
        }
        switch (field->type) {
        case NB_CODEC_JSON_CHARS: {
            size_t cap = field->param.array.elem_count;
            size_t n;
            if (cJSON_IsNull(item)) {
                if (cap != 0) {
                    ((char*)dest)[0] = '\0';
                }
                break;
            }
            if (!cJSON_IsString(item) || !item->valuestring) {
                return NB_CODEC_TYPE_MISMATCH;
            }
            n = strlen(item->valuestring);
            if (n + 1u > cap) {
                return NB_CODEC_LIMIT_EXCEEDED;
            }
            memcpy(dest, item->valuestring, n);
            ((char*)dest)[n] = '\0';
            break;
        }
        case NB_CODEC_JSON_ARRAY:
            status = nb_decode_array(item, dest, field);
            if (status != NB_CODEC_OK) {
                return status;
            }
            break;
        case NB_CODEC_JSON_VEC:
            status = nb_decode_vec(item, (nb_codec_json_vec_t*)dest, field);
            if (status != NB_CODEC_OK) {
                return status;
            }
            break;
        case NB_CODEC_JSON_MAP:
            status = nb_decode_map(item, (nb_codec_json_map_t*)dest, field);
            if (status != NB_CODEC_OK) {
                return status;
            }
            break;
        default:
            status = nb_decode_elem(item, field->type, dest, field->param.sub.model,
                                 field->param.sub.count);
            if (status != NB_CODEC_OK) {
                return status;
            }
            break;
        }
    }
    return NB_CODEC_OK;
}

static int nb_should_skip_field(const void* obj, const nb_codec_json_model_t* field) {
    const void* dest;

    if (field->presence_offset != 0) {
        return *(const uint8_t*)nb_field_cptr(obj, field->presence_offset) == 0;
    }
    dest = nb_field_cptr(obj, field->offset);
    if (field->type == NB_CODEC_JSON_STRING || field->type == NB_CODEC_JSON_RAW ||
        field->type == NB_CODEC_JSON_STRUCT) {
        return *(void* const*)dest == NULL;
    }
    return 0;
}

static int nb_encode_elem(const void* src, nb_codec_json_type_t type, const nb_codec_json_model_t* sub,
                       size_t sub_n, cJSON** out);

static int nb_encode_array(const void* base, const nb_codec_json_model_t* field, size_t count,
                        cJSON** out) {
    size_t i;
    size_t elem_size;
    cJSON* array;
    int status;

    status = nb_model_elem_size(field, &elem_size);
    if (status != NB_CODEC_OK) {
        return status;
    }
    array = cJSON_CreateArray();
    if (!array) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    for (i = 0; i < count; i++) {
        cJSON* item = NULL;
        status = nb_encode_elem((const char*)base + i * elem_size, field->param.array.elem_type,
                             field->param.array.model, field->param.array.model_count, &item);
        if (status != NB_CODEC_OK || !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            return status != NB_CODEC_OK ? status : NB_CODEC_OUT_OF_MEMORY;
        }
    }
    *out = array;
    return NB_CODEC_OK;
}

static int nb_encode_elem(const void* src, nb_codec_json_type_t type, const nb_codec_json_model_t* sub,
                       size_t sub_n, cJSON** out) {
    cJSON* item = NULL;

    switch (type) {
    case NB_CODEC_JSON_BOOL: {
        uint8_t v;
        memcpy(&v, src, 1);
        item = cJSON_CreateBool(v ? 1 : 0);
        break;
    }
    case NB_CODEC_JSON_I8: {
        int8_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_I16: {
        int16_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_I32: {
        int32_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_I64: {
        int64_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_U8: {
        uint8_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_U16: {
        uint16_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_U32: {
        uint32_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_U64: {
        uint64_t v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_F32: {
        float v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber((double)v);
        break;
    }
    case NB_CODEC_JSON_F64: {
        double v;
        memcpy(&v, src, sizeof(v));
        item = cJSON_CreateNumber(v);
        break;
    }
    case NB_CODEC_JSON_STRING: {
        const char* s = *(char* const*)src;
        if (!s) {
            item = cJSON_CreateNull();
            break;
        }
        item = cJSON_CreateString(s);
        break;
    }
    case NB_CODEC_JSON_RAW: {
        const char* s = *(char* const*)src;
        cJSON* parsed;
        if (!s) {
            item = cJSON_CreateNull();
            break;
        }
        parsed = cJSON_ParseWithOpts(s, NULL, 1);
        if (!parsed) {
            return NB_CODEC_MALFORMED;
        }
        *out = parsed;
        return NB_CODEC_OK;
    }
    case NB_CODEC_JSON_STRUCT: {
        const void* nested = *(const void* const*)src;
        if (!nested) {
            item = cJSON_CreateNull();
            break;
        }
        return nb_encode_object(nested, sub, sub_n, out);
    }
    case NB_CODEC_JSON_INLINE:
        return nb_encode_object(src, sub, sub_n, out);
    default:
        return NB_CODEC_INVALID_ARGUMENT;
    }

    if (!item) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    *out = item;
    return NB_CODEC_OK;
}

static int nb_encode_map(const nb_codec_json_map_t* map, const nb_codec_json_model_t* field, cJSON** out) {
    cJSON* root;
    size_t i;

    if (map->count != 0 && !map->entries) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    root = cJSON_CreateObject();
    if (!root) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    for (i = 0; i < map->count; i++) {
        cJSON* item = NULL;
        int status;

        if (!map->entries[i].key || !map->entries[i].value) {
            cJSON_Delete(root);
            return NB_CODEC_INVALID_ARGUMENT;
        }
        status = nb_encode_object(map->entries[i].value, field->param.sub.model,
                               field->param.sub.count, &item);
        if (status != NB_CODEC_OK || !cJSON_AddItemToObject(root, map->entries[i].key, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return status != NB_CODEC_OK ? status : NB_CODEC_OUT_OF_MEMORY;
        }
    }
    *out = root;
    return NB_CODEC_OK;
}

static int nb_encode_object(const void* obj, const nb_codec_json_model_t* model, size_t model_count,
                         cJSON** out) {
    cJSON* root;
    size_t i;

    if (!obj) {
        root = cJSON_CreateNull();
        if (!root) {
            return NB_CODEC_OUT_OF_MEMORY;
        }
        *out = root;
        return NB_CODEC_OK;
    }
    root = cJSON_CreateObject();
    if (!root) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    for (i = 0; i < model_count; i++) {
        const nb_codec_json_model_t* field = &model[i];
        const void* dest;
        cJSON* item = NULL;
        int status;

        if (field->type == NB_CODEC_JSON_OBJ) {
            continue;
        }
        if (!field->key) {
            cJSON_Delete(root);
            return NB_CODEC_INVALID_ARGUMENT;
        }
        if (nb_should_skip_field(obj, field)) {
            continue;
        }
        dest = nb_field_cptr(obj, field->offset);
        switch (field->type) {
        case NB_CODEC_JSON_CHARS: {
            const char* s = (const char*)dest;
            size_t cap = field->param.array.elem_count;
            size_t n = 0;
            char* tmp;
            while (n < cap && s[n] != '\0') {
                n++;
            }
            tmp = (char*)malloc(n + 1u);
            if (!tmp) {
                status = NB_CODEC_OUT_OF_MEMORY;
                break;
            }
            if (n != 0) {
                memcpy(tmp, s, n);
            }
            tmp[n] = '\0';
            item = cJSON_CreateString(tmp);
            free(tmp);
            status = item ? NB_CODEC_OK : NB_CODEC_OUT_OF_MEMORY;
            break;
        }
        case NB_CODEC_JSON_ARRAY:
            status = nb_encode_array(dest, field, field->param.array.elem_count, &item);
            break;
        case NB_CODEC_JSON_VEC: {
            const nb_codec_json_vec_t* vec = (const nb_codec_json_vec_t*)dest;
            if (vec->count != 0 && !vec->items) {
                cJSON_Delete(root);
                return NB_CODEC_INVALID_ARGUMENT;
            }
            status = nb_encode_array(vec->items, field, vec->count, &item);
            break;
        }
        case NB_CODEC_JSON_MAP:
            status = nb_encode_map((const nb_codec_json_map_t*)dest, field, &item);
            break;
        default:
            status = nb_encode_elem(dest, field->type, field->param.sub.model, field->param.sub.count,
                                 &item);
            break;
        }
        if (status != NB_CODEC_OK || !cJSON_AddItemToObject(root, field->key, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return status != NB_CODEC_OK ? status : NB_CODEC_OUT_OF_MEMORY;
        }
    }
    *out = root;
    return NB_CODEC_OK;
}

static void nb_clear_array_items(void* base, size_t count, const nb_codec_json_model_t* field) {
    size_t i;
    size_t elem_size;

    if (!base || nb_model_elem_size(field, &elem_size) != NB_CODEC_OK) {
        return;
    }
    for (i = 0; i < count; i++) {
        void* elem = (char*)base + i * elem_size;
        switch (field->param.array.elem_type) {
        case NB_CODEC_JSON_STRING:
        case NB_CODEC_JSON_RAW:
            free(*(char**)elem);
            *(char**)elem = NULL;
            break;
        case NB_CODEC_JSON_STRUCT:
            if (*(void**)elem) {
                nb_clear_object(*(void**)elem, field->param.array.model,
                             field->param.array.model_count, 1);
                *(void**)elem = NULL;
            }
            break;
        case NB_CODEC_JSON_INLINE:
            nb_clear_object(elem, field->param.array.model, field->param.array.model_count, 0);
            break;
        default:
            break;
        }
    }
}

static void nb_clear_object(void* obj, const nb_codec_json_model_t* model, size_t model_count,
                         int free_root) {
    size_t i;

    if (!obj || !model) {
        return;
    }
    for (i = 0; i < model_count; i++) {
        const nb_codec_json_model_t* field = &model[i];
        void* dest;

        if (field->type == NB_CODEC_JSON_OBJ) {
            continue;
        }
        dest = nb_field_ptr(obj, field->offset);
        if (field->presence_offset != 0) {
            *(uint8_t*)nb_field_ptr(obj, field->presence_offset) = 0u;
        }
        switch (field->type) {
        case NB_CODEC_JSON_STRING:
        case NB_CODEC_JSON_RAW:
            free(*(char**)dest);
            *(char**)dest = NULL;
            break;
        case NB_CODEC_JSON_STRUCT:
            if (*(void**)dest) {
                nb_clear_object(*(void**)dest, field->param.sub.model, field->param.sub.count, 1);
                *(void**)dest = NULL;
            }
            break;
        case NB_CODEC_JSON_INLINE:
            nb_clear_object(dest, field->param.sub.model, field->param.sub.count, 0);
            break;
        case NB_CODEC_JSON_ARRAY:
            nb_clear_array_items(dest, field->param.array.elem_count, field);
            break;
        case NB_CODEC_JSON_VEC: {
            nb_codec_json_vec_t* vec = (nb_codec_json_vec_t*)dest;
            nb_clear_array_items(vec->items, vec->count, field);
            free(vec->items);
            vec->items = NULL;
            vec->count = 0;
            break;
        }
        case NB_CODEC_JSON_MAP:
            nb_clear_map((nb_codec_json_map_t*)dest, field);
            break;
        default:
            break;
        }
    }
    if (free_root) {
        free(obj);
    }
}

static void nb_clear_map(nb_codec_json_map_t* map, const nb_codec_json_model_t* field) {
    size_t i;

    if (!map) {
        return;
    }
    for (i = 0; i < map->count; i++) {
        free(map->entries[i].key);
        if (map->entries[i].value) {
            nb_clear_object(map->entries[i].value, field->param.sub.model,
                         field->param.sub.count, 1);
        }
    }
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
}

/*
 * Parse a JSON document, tolerating line ("//") and block comments so config
 * files can be self-documenting. cJSON's parser rejects comments, but
 * cJSON_Minify strips them (and insignificant whitespace) in place; it copies
 * string literals verbatim, so comment-looking text inside a value is kept.
 */
static int nb_codec_json_parse(const char* json, cJSON** out) {
    char* buffer;
    size_t length;

    *out = NULL;
    if (!json) {
        return NB_CODEC_MALFORMED;
    }
    length = strlen(json);
    buffer = (char*)malloc(length + 1u);
    if (!buffer) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    memcpy(buffer, json, length + 1u);
    cJSON_Minify(buffer);
    *out = cJSON_ParseWithOpts(buffer, NULL, 1);
    free(buffer);
    return *out ? NB_CODEC_OK : NB_CODEC_MALFORMED;
}

static int nb_decode_root(const char* json, const nb_codec_json_model_t* model, size_t model_count,
                       void* into, void** allocated) {
    cJSON* root;
    size_t size = 0;
    void* obj = into;
    int status;

    if (!json || !model || model_count == 0 || (into == NULL && allocated == NULL) ||
        (into != NULL && allocated != NULL)) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    status = nb_model_obj_size(model, model_count, &size);
    if (status != NB_CODEC_OK) {
        return status;
    }
    status = nb_codec_json_parse(json, &root);
    if (status != NB_CODEC_OK) {
        return status;
    }
    if (cJSON_IsNull(root)) {
        cJSON_Delete(root);
        if (allocated) {
            *allocated = NULL;
        }
        return NB_CODEC_OK;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NB_CODEC_TYPE_MISMATCH;
    }
    if (!obj) {
        obj = calloc(1, size);
        if (!obj) {
            cJSON_Delete(root);
            return NB_CODEC_OUT_OF_MEMORY;
        }
    }
    status = nb_fill_object(root, obj, model, model_count);
    cJSON_Delete(root);
    if (status != NB_CODEC_OK) {
        nb_clear_object(obj, model, model_count, allocated != NULL);
        return status;
    }
    if (allocated) {
        *allocated = obj;
    }
    return NB_CODEC_OK;
}

int nb_codec_json_decode(const char* json, const nb_codec_json_model_t* model, size_t model_count,
                         void** obj) {
    if (!obj) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    *obj = NULL;
    return nb_decode_root(json, model, model_count, NULL, obj);
}

int nb_codec_json_decode_into(const char* json, const nb_codec_json_model_t* model,
                              size_t model_count, void* obj) {
    if (!obj) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    return nb_decode_root(json, model, model_count, obj, NULL);
}

int nb_codec_json_encode(const void* obj, const nb_codec_json_model_t* model, size_t model_count,
                         int pretty, char** json) {
    cJSON* root = NULL;
    char* text;
    size_t size;
    int status;

    if (!json || !model || model_count == 0) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    *json = NULL;
    status = nb_model_obj_size(model, model_count, &size);
    if (status != NB_CODEC_OK) {
        return status;
    }
    (void)size;
    status = nb_encode_object(obj, model, model_count, &root);
    if (status != NB_CODEC_OK) {
        cJSON_Delete(root);
        return status;
    }
    text = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return NB_CODEC_OUT_OF_MEMORY;
    }
    *json = text;
    return NB_CODEC_OK;
}

void nb_codec_json_clear(void* obj, const nb_codec_json_model_t* model, size_t model_count) {
    nb_clear_object(obj, model, model_count, 0);
}

void nb_codec_json_free(void* obj, const nb_codec_json_model_t* model, size_t model_count) {
    nb_clear_object(obj, model, model_count, 1);
}

void nb_codec_json_free_string(char* json) {
    cJSON_free(json);
}

int nb_codec_json_fill_object(const struct cJSON* node, const nb_codec_json_model_t* model,
                              size_t model_count, void* obj) {
    if (!node || !obj) {
        return NB_CODEC_INVALID_ARGUMENT;
    }
    return nb_fill_object(node, obj, model, model_count);
}
