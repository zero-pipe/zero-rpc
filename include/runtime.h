#ifndef NB_RUNTIME_H
#define NB_RUNTIME_H

/*
 * rpc 运行时 API。
 *
 * 这一层负责节点的生命周期、线程调度、服务发现、路由和运行指标。
 * 业务侧通常会在它之上再选择 server.h 或 client.h。
 */

#include <types.h>
#include <metrics.h>
#include <node.h>

#endif /* NB_RUNTIME_H */
