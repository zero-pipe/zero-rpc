#ifndef ZRPC_H
#define ZRPC_H

/*
 * zrpc - 基于 zero-tool-kit 的异步 RPC，支持 UDP（RTP/ARQ 可靠传输）与 TCP。
 *
 * 这是伞头文件：包含全部对外接口。也可以按需只包含某个子头。
 *
 * 设计约束：
 *  - 只提供异步接口，无阻塞/同步等待 API；
 *  - 不创建自有线程，全部运行在 ztk_poller_pool 线程上；
 *  - payload 对框架透明，序列化扩展点留给应用（见 zrpc_codec.h）。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_metrics.h>
#include <zrpc/zrpc_endpoint.h>
#include <zrpc/zrpc_transport_provider.h>
#include <zrpc/zrpc_codec.h>
#include <zrpc/zrpc_node.h>
#include <zrpc/zrpc_service.h>
#include <zrpc/zrpc_proxy.h>
#include <zrpc/zrpc_config.h>

#endif /* ZRPC_H */
