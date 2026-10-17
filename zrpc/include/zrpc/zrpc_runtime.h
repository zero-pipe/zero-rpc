#ifndef ZRPC_RUNTIME_H
#define ZRPC_RUNTIME_H

/*
 * zrpc 运行时 API。
 *
 * 这一层负责节点的生命周期、线程调度、服务发现、路由和运行指标。
 * 业务侧通常会在它之上再选择 zrpc_server.h 或 zrpc_client.h。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_metrics.h>
#include <zrpc/zrpc_node.h>

#endif /* ZRPC_RUNTIME_H */
