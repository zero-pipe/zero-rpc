#ifndef NB_MESSAGE_VIEW_H
#define NB_MESSAGE_VIEW_H

/*
 * 统一消息视图。
 *
 * 对齐 fastcall-c 的 fc_message_view：请求与响应共用同一个结构承载 payload，
 * 只是语义不同。视图可以借用外部内存（borrowed），也可以用 set_data /
 * set_string 拥有自己的拷贝（owned）；owned 缓冲由 nb_message_view_release
 * 释放。
 *
 * 主要用途：同步风格 handler（见 nb_service_add_sync_method）在返回前把响应
 * 写进 response view，框架负责在回调返回后释放它。
 */

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 视图结构 ---- */
typedef struct nb_message_view {
    const void *data; /* 字节视图 */
    size_t len;       /* 字节数 */
    uint8_t encoding; /* 应用编码标识，0 = 裸字节 */
    void *owned;      /* 内部：set_* 分配的所有权缓冲，勿手动修改 */
} nb_message_view_t;

/* ---- 生命周期 ---- */
/* 初始化为空视图。 */
void nb_message_view_init(nb_message_view_t *view);
/* 释放 owned 缓冲并清空视图。借用视图调用它也安全。 */
void nb_message_view_release(nb_message_view_t *view);

/* ---- 写入 ---- */
/* 借用外部内存（不拷贝、不接管生命周期）。 */
int nb_message_view_set_borrowed(nb_message_view_t *view, const void *data, size_t len);

/* 拷贝一份内存，由视图拥有；重复调用会替换旧缓冲。 */
int nb_message_view_set_data(nb_message_view_t *view, const void *data, size_t len);
/* 拷贝 NUL 结尾字符串（不含结尾 '\0'）进视图。 */
int nb_message_view_set_string(nb_message_view_t *view, const char *text);
/* 按 payload 设置视图；设置 encoding，data 拷贝进视图。 */
int nb_message_view_set_payload(nb_message_view_t *view, const nb_payload_t *payload);

/* ---- 导出 ---- */
/* 导出为 payload（data 指向视图缓冲，仅在视图存续期内有效）。 */
void nb_message_view_to_payload(const nb_message_view_t *view, nb_payload_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_MESSAGE_VIEW_H */
