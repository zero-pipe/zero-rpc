#ifndef NB_SERVER_H
#define NB_SERVER_H

/*
 * rpc 服务端 API。
 *
 * 包含节点运行时和 service 注册/回复接口。只做客户端的程序可以改用
 * client.h，避免把服务端类型带进自己的公共头文件。
 */

#include <runtime.h>
#include <message_view.h>
#include <service.h>

#endif /* NB_SERVER_H */
