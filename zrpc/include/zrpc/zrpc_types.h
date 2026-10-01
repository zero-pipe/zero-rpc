#ifndef ZRPC_TYPES_H
#define ZRPC_TYPES_H

/*
 * zrpc 公共基础类型。
 *
 * 分层约定：
 *   - 传输层（UDP/TCP）只搬运字节，不解释业务语义；
 *   - 应用层的 payload 对框架完全透明（见 zrpc_payload_t）；
 *   - 所有回调都在 ztk poller 线程内触发，回调中禁止阻塞。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 状态码 ---- */
#define ZRPC_OK 0
#define ZRPC_ERR_INVALID (-1)
#define ZRPC_ERR_NOMEM (-2)
#define ZRPC_ERR_NOTFOUND (-3) /* 服务未发现 / 请求已失效 */
#define ZRPC_ERR_TIMEOUT (-4)
#define ZRPC_ERR_IO (-5)
#define ZRPC_ERR_STATE (-6)
#define ZRPC_ERR_AGAIN (-7)
#define ZRPC_ERR_TOOBIG (-8)
#define ZRPC_ERR_CLOSED (-9)

/* 不透明句柄 */
typedef struct zrpc_node zrpc_node_t;
typedef struct zrpc_service zrpc_service_t;
typedef struct zrpc_proxy zrpc_proxy_t;
typedef struct zrpc_call zrpc_call_t;
typedef struct zrpc_stream zrpc_stream_t;

/* 流式 chunk 标志 */
#define ZRPC_STREAM_FIRST 0x01u
#define ZRPC_STREAM_LAST 0x02u
#define ZRPC_STREAM_CANCEL 0x04u

/* 单向数据 chunk；data 仅在回调/发送调用期间有效。 */
typedef struct zrpc_chunk {
    const void *data;
    size_t len;
    uint8_t encoding;
    uint64_t offset;
    unsigned flags;
} zrpc_chunk_t;

/* 传输层透传的 stream 元数据；unary 消息为 NULL。 */
typedef struct zrpc_stream_meta {
    unsigned flags;      /* ZRPC_STREAM_FIRST / ZRPC_STREAM_LAST */
    uint32_t stream_id;
    uint64_t offset;
    uint64_t total_size;
} zrpc_stream_meta_t;

/* 传输种类。节点创建时二选一，同一节点使用同一种传输。 */
typedef enum zrpc_transport_kind {
    ZRPC_TRANSPORT_UDP = 0,
    ZRPC_TRANSPORT_TCP = 1,
    ZRPC_TRANSPORT_CUSTOM = 255
} zrpc_transport_kind_t;

/* 节点发现模式：MESH 广播发现；STATIC 只用静态路由，不启动发现。 */
typedef enum zrpc_node_mode {
    ZRPC_MODE_MESH = 0,
    ZRPC_MODE_STATIC = 1
} zrpc_node_mode_t;

/*
 * 应用 payload 视图（对框架不透明）。
 *
 * data/len 是原始字节；encoding 是应用自定义的编码标识（0 表示裸字节）。
 * 框架只透传，不做任何序列化/反序列化；用户可注册 codec（见 zrpc_codec.h）
 * 把业务对象与 payload 互转，从而把序列化扩展点留给应用层。
 *
 * 生命周期：回调参数里的 payload 仅在本次回调内有效，需要跨回调保存时请自行拷贝。
 */
typedef struct zrpc_payload {
    const void *data;
    size_t len;
    uint8_t encoding;
} zrpc_payload_t;

/* 默认发现端口（广播） */
#ifndef ZRPC_DISCOVERY_PORT
#define ZRPC_DISCOVERY_PORT 41953
#endif

/* 默认异步调用超时 */
#ifndef ZRPC_DEFAULT_TIMEOUT_MS
#define ZRPC_DEFAULT_TIMEOUT_MS 5000
#endif

/* 名称与地址长度上限 */
#define ZRPC_NAME_MAX 64
#define ZRPC_SERVICE_NAME_MAX 64
#define ZRPC_METHOD_NAME_MAX 64
#define ZRPC_ENDPOINT_MAX 64
#define ZRPC_SCHEME_MAX 16
#define ZRPC_MAX_BINDINGS 8
#define ZRPC_MAX_ENDPOINTS 32

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_TYPES_H */
