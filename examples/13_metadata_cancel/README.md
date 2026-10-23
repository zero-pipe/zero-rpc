# 13：Metadata、Deadline 与取消

演示三件事：

1. 客户端通过 `_ex` 接口发送请求 metadata，服务端读取并用 `nb_reply_ex` 返回响应 metadata。
2. handler 通过 `nb_call_deadline_ms` 读取客户端 timeout 换算出的绝对 deadline。
3. 客户端用 `nb_proxy_cancel` 取消一个正在执行的慢调用，本地回调得到 `NB_ERR_NOTFOUND`。

```powershell
.\example_13_metadata_cancel.exe
```

取消是 best-effort：已经运行的 handler 不会被抢占，只能通过
`nb_call_canceled` 协作退出。
