#ifndef NB_CLIENT_H
#define NB_CLIENT_H

/*
 * rpc 客户端 API。
 *
 * proxy 是带服务发现/路由的高层接口，stub 是传输无关的底层调用原语。
 * 同步调用也属于 proxy API，声明在 proxy.h 中。
 */

#include <runtime.h>
#include <proxy.h>
#include <stub.h>

#endif /* NB_CLIENT_H */
