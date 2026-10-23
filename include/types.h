#ifndef NB_TYPES_H
#define NB_TYPES_H

/*
 * rpc 公共基础类型。
 *
 * 分层约定：
 *   - 传输层（UDP/TCP）只搬运字节，不解释业务语义；
 *   - 应用层的 payload 对框架完全透明（见 nb_payload_t）；
 *   - 客户端响应回调与 INLINE handler 在 ntk poller 线程触发，禁止阻塞；
 *     WORKER / EXECUTOR handler 在对应执行线程运行（见 service.h）。
 */

#include <stddef.h>
#include <stdint.h>

#include <error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 消息标志 ---- */
/*
 * 响应包的 status 字段承载框架错误码（如 NB_ERR_AGAIN），而非应用状态。
 * 应用状态码仍只通过 nb_response_t.status 传递。
 */
#define NB_MSG_FLAG_FRAMEWORK_STATUS 0x01u

/* ---- 元数据（对齐 gRPC metadata：字符串 key + 二进制 value） ---- */
#define NB_METADATA_MAX_ENTRIES 16
#define NB_METADATA_KEY_MAX 64
#define NB_METADATA_VALUE_MAX 256
#define NB_METADATA_WIRE_MAX 4096
#define NB_STATUS_MSG_MAX 512

typedef struct nb_metadata_entry {
    char key[NB_METADATA_KEY_MAX];
    uint8_t value[NB_METADATA_VALUE_MAX];
    uint16_t value_len;
} nb_metadata_entry_t;

typedef struct nb_metadata {
    uint16_t count;
    nb_metadata_entry_t entries[NB_METADATA_MAX_ENTRIES];
} nb_metadata_t;

void nb_metadata_init(nb_metadata_t *metadata);
int nb_metadata_set(nb_metadata_t *metadata, const char *key, const void *value, size_t len);
int nb_metadata_set_string(nb_metadata_t *metadata, const char *key, const char *value);
const void *nb_metadata_get(const nb_metadata_t *metadata, const char *key, size_t *len);
const char *nb_metadata_get_string(const nb_metadata_t *metadata, const char *key);
int nb_metadata_remove(nb_metadata_t *metadata, const char *key);
int nb_metadata_encode(const nb_metadata_t *metadata, void *buf, size_t cap, size_t *out_len);
int nb_metadata_decode(nb_metadata_t *metadata, const void *buf, size_t len);

/* ---- 不透明句柄 ---- */
typedef struct nb_node nb_node_t;
typedef struct nb_service nb_service_t;
typedef struct nb_proxy nb_proxy_t;
typedef struct nb_call nb_call_t;
typedef struct nb_stream nb_stream_t;

/* ---- 流式消息 ---- */
#define NB_STREAM_FIRST 0x01u
#define NB_STREAM_LAST 0x02u
#define NB_STREAM_CANCEL 0x04u

/* 单向数据 chunk；data 仅在回调/发送调用期间有效。 */
typedef struct nb_chunk {
    const void *data;
    size_t len;
    uint64_t offset;
    const nb_metadata_t *metadata; /* 可为 NULL；仅在本次回调内有效 */
    const char *message;        /* 状态消息，可为 NULL */
    unsigned flags;
    uint8_t encoding;
} nb_chunk_t;

/* 传输层透传的 stream 元数据；unary 消息为 NULL。 */
typedef struct nb_stream_meta {
    unsigned flags; /* NB_STREAM_FIRST / NB_STREAM_LAST */
    uint32_t stream_id;
    uint64_t offset;
    uint64_t total_size;
} nb_stream_meta_t;

/* ---- 传输与节点模式 ---- */
/* 传输种类。默认 binding 使用 UDP；显式 bindings 可按 scheme 选择不同传输。 */
typedef enum nb_transport_kind {
    NB_TRANSPORT_UDP = 0,
    NB_TRANSPORT_TCP = 1,
    NB_TRANSPORT_CUSTOM = 255
} nb_transport_kind_t;

/* Optional transport-local fast path for event/pub-sub delivery. */
typedef enum nb_intra_process {
    NB_INTRA_OFF = 0,
    NB_INTRA_AUTO = 1,
    NB_INTRA_ONLY = 2
} nb_intra_process_t;

/* 节点发现模式：MESH 广播发现；STATIC 只用静态路由，不启动发现。 */
typedef enum nb_node_mode {
    NB_MODE_MESH = 0,
    NB_MODE_STATIC = 1
} nb_node_mode_t;

/* ---- 应用消息视图 ---- */
/*
 * 应用 payload 视图（对框架不透明）。
 *
 * data/len 是原始字节；encoding 是应用自定义的编码标识（0 表示裸字节）。
 * 框架只透传，不做任何序列化/反序列化；用户可注册 codec（见 codec.h）
 * 把业务对象与 payload 互转，从而把序列化扩展点留给应用层。
 *
 * 生命周期：回调参数里的 payload 仅在本次回调内有效，需要跨回调保存时请自行拷贝。
 *
 * 发送侧所有权：nb_proxy_call_async / nb_stub_call_async / nb_proxy_call_event 会在
 * 返回前复制 payload（跨线程调用时同样如此），调用方始终保留自己缓冲区的
 * 所有权，可在调用返回后立即释放，无需等回调。框架不会 free 调用方内存。
 * 服务端 nb_reply() 也只在调用期间借用响应 payload，返回后即可释放。
 */
typedef struct nb_payload {
    const void *data;
    size_t len;
    uint8_t encoding;
} nb_payload_t;

/*
 * 请求/响应视图（对框架不透明）。请求带有路由与对端信息；响应带有应用状态码
 * 与 payload。两者共享同一个 nb_payload_t 视图，对齐 fastcall-c 的
 * fc_message_view 设计。
 *
 * 生命周期：回调参数里的视图（含 payload）仅在本次回调内有效。
 */
typedef struct nb_request {
    const char *service;
    const char *method;
    nb_payload_t payload; /* 对框架透明的业务字节 */
    const char *peer_ip;
    uint16_t peer_port;
    /*
     * 请求 deadline（绝对单调毫秒，0 = 无 deadline），由客户端的 timeout_ms
     * 在接收时换算而来（对齐 gRPC deadline 传播）。服务端 handler 可据此提前
     * 放弃注定会被客户端丢弃的重活；框架也会在 handler 启动前跳过已过期请求。
     */
    uint64_t deadline_ms;
    const nb_metadata_t *metadata; /* 请求 metadata，可为 NULL；仅本次回调内有效 */
} nb_request_t;

typedef struct nb_response {
    int status;          /* 应用定义，0 建议表示成功 */
    const char *message; /* 状态消息，可为 NULL；仅回调内有效 */
    nb_payload_t payload;
    const nb_metadata_t *metadata; /* 响应 metadata，可为 NULL；仅回调内有效 */
} nb_response_t;

/* ---- 回调 ---- */
/* 客户端响应回调。status：NB_OK 或错误码；resp 仅在回调内有效。 */
typedef void (*response_fn)(int status, const nb_response_t *resp, void *user);
typedef void (*response_chunk_fn)(int status, const nb_chunk_t *chunk, void *user);
typedef void (*stream_done_fn)(int status, void *user);

/* ---- 默认值与容量 ---- */
/* 默认发现端口（广播） */
#ifndef NB_DISCOVERY_PORT
#define NB_DISCOVERY_PORT 41953
#endif

/* 默认异步调用超时 */
#ifndef NB_DEFAULT_TIMEOUT_MS
#define NB_DEFAULT_TIMEOUT_MS 5000
#endif

/* 名称与地址长度上限 */
#define NB_NAME_MAX 64
#define NB_SERVICE_NAME_MAX 64
#define NB_METHOD_NAME_MAX 64
#define NB_ENDPOINT_MAX 64
#define NB_SCHEME_MAX 16
#define NB_MAX_BINDINGS 8
#define NB_MAX_ENDPOINTS 32

#ifdef __cplusplus
}
#endif

#endif /* NB_TYPES_H */
