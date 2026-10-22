#ifndef NB_TRANSPORT_H
#define NB_TRANSPORT_H

/*
 * 传输与端点扩展 API。
 *
 * endpoint 用于解析/格式化 scheme://host:port；transport provider 用于接入
 * 自定义传输。UDP/TCP 的具体实现不属于这个公共头文件。
 *
 * 安全边界：内置 UDP/TCP 传输是明文、无认证的。envelope 只做 magic/version/
 * CRC 完整性校验（防误码与串包），不提供机密性/身份保证，也不应直接暴露在
 * 不可信网络。需要 TLS/DTLS 或应用层鉴权时：
 *   - 用自定义 transport provider 承载加密信道（见 transport_provider.h），或
 *   - 在 nb_service_set_interceptor / handler 里校验 metadata 中的凭证，并用
 *     peer_rate_limit_pps / max_streams 做基本流控与资源上限。
 */

#include <endpoint.h>
#include <transport_provider.h>

#endif /* NB_TRANSPORT_H */
