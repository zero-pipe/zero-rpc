#ifndef ZRPC_TRANSPORT_H
#define ZRPC_TRANSPORT_H

/*
 * 传输与端点扩展 API。
 *
 * endpoint 用于解析/格式化 scheme://host:port；transport provider 用于接入
 * 自定义传输。UDP/TCP 的具体实现不属于这个公共头文件。
 */

#include <zrpc/zrpc_endpoint.h>
#include <zrpc/zrpc_transport_provider.h>

#endif /* ZRPC_TRANSPORT_H */
