# 12：Worker 线程池

服务端配置 `runtime.handler_threads = 2`，把会阻塞的 handler 注册为
`NB_SERVICE_EXEC_WORKER`。handler 在 worker 线程 sleep，poller 仍然可以处理
网络事件，回复自动回投原 poller。

```powershell
.\example_12_worker.exe
```

对比：如果同一个 handler 使用默认 INLINE 模式，sleep 会卡住整条 poller。
