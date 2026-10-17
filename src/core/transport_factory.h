#ifndef TRANSPORT_FACTORY_H
#define TRANSPORT_FACTORY_H

/*
 * 传输装配：为 node 的一个 binding 创建/启动传输（UDP/TCP 或自定义 provider），
 * 并登记到 node->transports[]。从 node.c 拆出，聚焦“装配”职责。
 */

#include <types.h>

#include <stdint.h>

/* 成功返回 OK；scheme 未注册/参数非法/启动失败返回相应错误码。 */
int zri_node_transport_add(node_t *node, const char *scheme, const char *host, uint16_t port);

#endif /* TRANSPORT_FACTORY_H */
