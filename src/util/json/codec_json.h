#ifndef NB_CODEC_JSON_H
#define NB_CODEC_JSON_H

/*
 * 模型驱动的 JSON 编解码（network bridge 公共原语）。
 *
 * 用一组静态 model 描述 C 结构体的字段布局，即可把 JSON 填充进结构体，
 * 或把结构体序列化成 JSON。配置模块只负责写 model，不再手写逐字段解析。
 *
 * 所有权：
 *   - decode(..., &obj)：框架分配 obj（及其内部 string/vec/map），用 free 回收。
 *   - decode_into(..., obj)：填充调用方结构体，内部动态内存用 clear 回收。
 *   - encode(..., &json)：返回堆字符串，用 free_string 回收。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nb_codec_json_status {
    NB_CODEC_OK = 0,
    NB_CODEC_INVALID_ARGUMENT = -1,
    NB_CODEC_TYPE_MISMATCH = -2,
    NB_CODEC_LIMIT_EXCEEDED = -3,
    NB_CODEC_MALFORMED = -4,
    NB_CODEC_OUT_OF_MEMORY = -5
} nb_codec_json_status_t;

typedef enum nb_codec_json_type {
    NB_CODEC_JSON_OBJ = 0, /* 对象根/内嵌对象描述符，param.obj_size = 结构体大小 */
    NB_CODEC_JSON_BOOL,
    NB_CODEC_JSON_I8,
    NB_CODEC_JSON_U8,
    NB_CODEC_JSON_I16,
    NB_CODEC_JSON_U16,
    NB_CODEC_JSON_I32,
    NB_CODEC_JSON_U32,
    NB_CODEC_JSON_I64,
    NB_CODEC_JSON_U64,
    NB_CODEC_JSON_F32,
    NB_CODEC_JSON_F64,
    NB_CODEC_JSON_STRING, /* char*，框架分配 */
    NB_CODEC_JSON_RAW,    /* 原始 JSON 子树，序列化为 char* */
    NB_CODEC_JSON_STRUCT, /* 嵌套对象，字段为指针，框架分配 */
    NB_CODEC_JSON_INLINE, /* 内嵌对象，字段为内联结构体 */
    NB_CODEC_JSON_CHARS,  /* char[N] 定长字符串，param.array.elem_count = 容量 */
    NB_CODEC_JSON_ARRAY,  /* 定长数组，param.array.* 描述元素 */
    NB_CODEC_JSON_VEC,    /* 变长数组 nb_codec_json_vec_t */
    NB_CODEC_JSON_MAP     /* 对象映射 nb_codec_json_map_t */
} nb_codec_json_type_t;

typedef struct nb_codec_json_vec {
    void *items;
    size_t count;
} nb_codec_json_vec_t;

typedef struct nb_codec_json_map_entry {
    char *key;
    void *value;
} nb_codec_json_map_entry_t;

typedef struct nb_codec_json_map {
    nb_codec_json_map_entry_t *entries;
    size_t count;
} nb_codec_json_map_t;

typedef struct nb_codec_json_model nb_codec_json_model_t;

struct nb_codec_json_model {
    nb_codec_json_type_t type;
    const char *key;      /* JSON 字段名；OBJ 根描述符可为 NULL */
    size_t offset;        /* 目标结构体字段偏移（OBJ 根忽略） */
    size_t presence_offset; /* 可选：字段出现时置 1 的 uint8_t 偏移，0 = 不跟踪 */
    union {
        size_t obj_size; /* OBJ：结构体大小 */
        struct {
            size_t elem_size;
            nb_codec_json_type_t elem_type;
            size_t elem_count;
            const nb_codec_json_model_t *model;
            size_t model_count;
        } array;
        struct {
            const nb_codec_json_model_t *model;
            size_t count;
        } sub;
    } param;
};

/* ---- 编解码 ---- */
int nb_codec_json_decode(const char *json,
                         const nb_codec_json_model_t *model,
                         size_t model_count,
                         void **obj);
int nb_codec_json_decode_into(const char *json,
                              const nb_codec_json_model_t *model,
                              size_t model_count,
                              void *obj);
int nb_codec_json_encode(const void *obj,
                         const nb_codec_json_model_t *model,
                         size_t model_count,
                         int pretty,
                         char **json);

/* ---- 释放 ---- */
void nb_codec_json_clear(void *obj, const nb_codec_json_model_t *model, size_t model_count);
void nb_codec_json_free(void *obj, const nb_codec_json_model_t *model, size_t model_count);
void nb_codec_json_free_string(char *json);

#ifdef __cplusplus
}
#endif

#endif /* NB_CODEC_JSON_H */
