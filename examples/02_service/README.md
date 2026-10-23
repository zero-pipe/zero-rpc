# 02：Service 与同步 handler

启动一个 TCP 服务端，注册 `HealthService.check`。handler 使用同步风格：
直接写 `nb_message_view_t` 并返回应用状态码，框架负责异步回复。

```powershell
.\example_02_service.exe [port]
```

默认监听 `tcp://127.0.0.1:42410`，保持运行后用
[`05_sync_call`](../05_sync_call/README.md) 调用。
