#ifndef ZRPC_SERVER_H
#define ZRPC_SERVER_H

/*
 * zrpc 服务端 API。
 *
 * 包含节点运行时和 service 注册/回复接口。只做客户端的程序可以改用
 * zrpc_client.h，避免把服务端类型带进自己的公共头文件。
 */

#include <zrpc/zrpc_runtime.h>
#include <zrpc/zrpc_message_view.h>
#include <zrpc/zrpc_service.h>

#endif /* ZRPC_SERVER_H */
