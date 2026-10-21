#ifndef TYPES_H
#define TYPES_H

/*
 * rpc 公共基础类型。
 *
 * 分层约定：
 *   - 传输层（UDP/TCP）只搬运字节，不解释业务语义；
 *   - 应用层的 payload 对框架完全透明（见 payload_t）；
 *   - 客户端响应回调与 INLINE handler 在 ntk poller 线程触发，禁止阻塞；
 *     WORKER / EXECUTOR handler 在对应执行线程运行（见 service.h）。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 状态码 ---- */
#define OK 0
#define ERR_INVALID (-1)
#define ERR_NOMEM (-2)
#define ERR_NOTFOUND (-3) /* 服务未发现 / 请求已失效 */
#define ERR_TIMEOUT (-4)
#define ERR_IO (-5)
#define ERR_STATE (-6)
#define ERR_AGAIN (-7)
#define ERR_TOOBIG (-8)
#define ERR_CLOSED (-9)

/* ---- 元数据（对齐 gRPC metadata：字符串 key + 二进制 value） ---- */
#define METADATA_MAX_ENTRIES 16
#define METADATA_KEY_MAX 64
#define METADATA_VALUE_MAX 256
#define METADATA_WIRE_MAX 4096
#define STATUS_MSG_MAX 512

typedef struct metadata_entry {
    char key[METADATA_KEY_MAX];
    uint8_t value[METADATA_VALUE_MAX];
    uint16_t value_len;
} metadata_entry_t;

typedef struct metadata {
    uint16_t count;
    metadata_entry_t entries[METADATA_MAX_ENTRIES];
} metadata_t;

void metadata_init(metadata_t *metadata);
int metadata_set(metadata_t *metadata, const char *key, const void *value, size_t len);
int metadata_set_string(metadata_t *metadata, const char *key, const char *value);
const void *metadata_get(const metadata_t *metadata, const char *key, size_t *len);
const char *metadata_get_string(const metadata_t *metadata, const char *key);
int metadata_remove(metadata_t *metadata, const char *key);
int metadata_encode(const metadata_t *metadata, void *buf, size_t cap, size_t *out_len);
int metadata_decode(metadata_t *metadata, const void *buf, size_t len);

/* ---- 不透明句柄 ---- */
typedef struct node node_t;
typedef struct service service_t;
typedef struct proxy proxy_t;
typedef struct call call_t;
typedef struct stream stream_t;

/* ---- 流式消息 ---- */
#define STREAM_FIRST 0x01u
#define STREAM_LAST 0x02u
#define STREAM_CANCEL 0x04u

/* 单向数据 chunk；data 仅在回调/发送调用期间有效。 */
typedef struct chunk {
    const void *data;
    size_t len;
    uint64_t offset;
    const metadata_t *metadata; /* 可为 NULL；仅在本次回调内有效 */
    const char *message;        /* 状态消息，可为 NULL */
    unsigned flags;
    uint8_t encoding;
} chunk_t;

/* 传输层透传的 stream 元数据；unary 消息为 NULL。 */
typedef struct stream_meta {
    unsigned flags;      /* STREAM_FIRST / STREAM_LAST */
    uint32_t stream_id;
    uint64_t offset;
    uint64_t total_size;
} stream_meta_t;

/* ---- 传输与节点模式 ---- */
/* 传输种类。默认 binding 使用 UDP；显式 bindings 可按 scheme 选择不同传输。 */
typedef enum transport_kind {
    TRANSPORT_UDP = 0,
    TRANSPORT_TCP = 1,
    TRANSPORT_CUSTOM = 255
} transport_kind_t;

/* 节点发现模式：MESH 广播发现；STATIC 只用静态路由，不启动发现。 */
typedef enum node_mode {
    MODE_MESH = 0,
    MODE_STATIC = 1
} node_mode_t;

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
 * 发送侧所有权：proxy_call_async / stub_call_async / proxy_call_event 会在
 * 返回前复制 payload（跨线程调用时同样如此），调用方始终保留自己缓冲区的
 * 所有权，可在调用返回后立即释放，无需等回调。框架不会 free 调用方内存。
 * 服务端 reply() 也只在调用期间借用响应 payload，返回后即可释放。
 */
typedef struct payload {
    const void *data;
    size_t len;
    uint8_t encoding;
} payload_t;

/*
 * 请求/响应视图（对框架不透明）。请求带有路由与对端信息；响应带有应用状态码
 * 与 payload。两者共享同一个 payload_t 视图，对齐 fastcall-c 的
 * fc_message_view 设计。
 *
 * 生命周期：回调参数里的视图（含 payload）仅在本次回调内有效。
 */
typedef struct request {
    const char *service;
    const char *method;
    payload_t payload; /* 对框架透明的业务字节 */
    const char *peer_ip;
    uint16_t peer_port;
    /*
     * 请求 deadline（绝对单调毫秒，0 = 无 deadline），由客户端的 timeout_ms
     * 在接收时换算而来（对齐 gRPC deadline 传播）。服务端 handler 可据此提前
     * 放弃注定会被客户端丢弃的重活；框架也会在 handler 启动前跳过已过期请求。
     */
    uint64_t deadline_ms;
    const metadata_t *metadata; /* 请求 metadata，可为 NULL；仅本次回调内有效 */
} request_t;

typedef struct response {
    int status;             /* 应用定义，0 建议表示成功 */
    const char *message;    /* 状态消息，可为 NULL；仅回调内有效 */
    payload_t payload;
    const metadata_t *metadata; /* 响应 metadata，可为 NULL；仅回调内有效 */
} response_t;

/* ---- 回调 ---- */
/* 客户端响应回调。status：OK 或错误码；resp 仅在回调内有效。 */
typedef void (*response_fn)(int status, const response_t *resp, void *user);
typedef void (*response_chunk_fn)(int status, const chunk_t *chunk, void *user);
typedef void (*stream_done_fn)(int status, void *user);

/* ---- 默认值与容量 ---- */
/* 默认发现端口（广播） */
#ifndef DISCOVERY_PORT
#define DISCOVERY_PORT 41953
#endif

/* 默认异步调用超时 */
#ifndef DEFAULT_TIMEOUT_MS
#define DEFAULT_TIMEOUT_MS 5000
#endif

/* 名称与地址长度上限 */
#define NAME_MAX 64
#define SERVICE_NAME_MAX 64
#define METHOD_NAME_MAX 64
#define ENDPOINT_MAX 64
#define SCHEME_MAX 16
#define MAX_BINDINGS 8
#define MAX_ENDPOINTS 32

#ifdef __cplusplus
}
#endif

#endif /* TYPES_H */
