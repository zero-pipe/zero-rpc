# 错误码

公共错误码定义在 [`include/error.h`](../../include/error.h)。`NB_ERROR_TABLE` 是唯一真源，
同时生成枚举值、稳定名称、描述和重试标记。旧版本已经使用的 `0..-9` 数值保持不变。

## 布局

| 数值 | 类别 |
| --- | --- |
| `0` | 成功 |
| `-1..-9` | 原始框架错误码，保持兼容 |
| `-10..-19` | 扩展框架错误码 |
| `<= -1000` | 应用自定义错误码 |

## 框架错误码

| 名称 | 数值 | 含义 | 可重试 |
| --- | ---: | --- | :---: |
| `NB_OK` | 0 | 成功 | 否 |
| `NB_ERR_INVALID` | -1 | 参数非法 | 否 |
| `NB_ERR_NOMEM` | -2 | 内存不足 | 否 |
| `NB_ERR_NOTFOUND` | -3 | 服务、方法、路由或请求不存在 | 否 |
| `NB_ERR_TIMEOUT` | -4 | 操作超时 | 否 |
| `NB_ERR_IO` | -5 | I/O 错误 | 否 |
| `NB_ERR_STATE` | -6 | 对象状态非法 | 否 |
| `NB_ERR_AGAIN` | -7 | 暂时不可用，可重试 | 是 |
| `NB_ERR_TOOBIG` | -8 | 消息或字段超过上限 | 否 |
| `NB_ERR_CLOSED` | -9 | 对象或连接已关闭 | 否 |
| `NB_ERR_ALREADY` | -10 | 已启动或条目已存在 | 否 |
| `NB_ERR_NOT_SUPPORTED` | -11 | 当前构建或传输不支持 | 否 |
| `NB_ERR_CANCELED` | -12 | 操作被取消 | 否 |
| `NB_ERR_OVERLOAD` | -13 | 服务端过载，稍后重试 | 是 |
| `NB_ERR_BAD_MESSAGE` | -14 | 报文或 payload 格式错误 | 否 |
| `NB_ERR_PERMISSION` | -15 | 权限不足 | 否 |
| `NB_ERR_ADDR_IN_USE` | -16 | 地址已被占用 | 否 |
| `NB_ERR_CONN_REFUSED` | -17 | 连接被拒绝 | 否 |
| `NB_ERR_CONN_RESET` | -18 | 连接被对端重置 | 否 |
| `NB_ERR_BUSY` | -19 | 资源忙，可重试 | 是 |
| `NB_ERR_USER_BASE` | -1000 | 应用错误码起始值 | 否 |

`NB_ERR_TIMEOUT` 本身不标记为可重试，因为重试可能重复执行非幂等业务。调用方应结合
业务幂等性和超时原因决定是否重试。

## 应用错误码

应用状态码通过 `nb_response_t.status` 和 `nb_reply(call, status, ...)` 传递。建议应用
使用 `NB_ERR_USER_BASE` 以下的负数，或自行约定非负数区间，避免与框架错误码冲突：

```c
enum {
    APP_ERR_ORDER_NOT_FOUND = -1001,
    APP_ERR_OUT_OF_STOCK = -1002
};
```

客户端回调同时会收到框架状态 `status` 和响应中的应用状态 `resp->status`。两者不要混用。

## 查询辅助函数

```c
#include <error.h>

int rc = nb_proxy_call_sync(...);
if (rc != NB_OK) {
    nb_log_error("call failed: %s (%d), retry=%d",
                 nb_error_name(rc), rc, nb_error_is_retryable(rc));
}
```

`nb_error_name()` 返回稳定符号名，例如 `NB_ERR_TIMEOUT`；`nb_strerror()` 返回人读描述；
`nb_error_is_retryable()` 返回非零表示可以重试。未知数值不会返回 `NULL`。

## 新增错误码

修改 `NB_ERROR_TABLE` 即可自动生成枚举和查询表。新增错误码必须使用未占用的数值，
不得改动已有数值，以免破坏调用方判断、日志统计或 wire 上的状态字段。
