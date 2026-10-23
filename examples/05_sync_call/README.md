# 05：同步调用

调用 [`02_service`](../02_service/README.md) 的 `HealthService.check`。
核心仍是异步 RPC，`nb_proxy_call_sync` 只是阻塞等待封装，禁止在 poller
回调中使用。

先启动服务端：

```powershell
.\example_02_service.exe
```

再运行客户端：

```powershell
.\example_05_sync_call.exe [port]
```

默认端口 `42410`，成功时打印服务端回显内容。
