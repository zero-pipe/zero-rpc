#ifndef ZRPC_CLIENT_H
#define ZRPC_CLIENT_H

/*
 * zrpc 客户端 API。
 *
 * proxy 是带服务发现/路由的高层接口，stub 是传输无关的底层调用原语。
 * 同步调用也属于 proxy API，声明在 zrpc_proxy.h 中。
 */

#include <zrpc/zrpc_runtime.h>
#include <zrpc/zrpc_proxy.h>
#include <zrpc/zrpc_stub.h>

#endif /* ZRPC_CLIENT_H */
