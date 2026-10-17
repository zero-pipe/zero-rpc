#ifndef TRANSPORT_H
#define TRANSPORT_H

/*
 * 传输与端点扩展 API。
 *
 * endpoint 用于解析/格式化 scheme://host:port；transport provider 用于接入
 * 自定义传输。UDP/TCP 的具体实现不属于这个公共头文件。
 */

#include <endpoint.h>
#include <transport_provider.h>

#endif /* TRANSPORT_H */
