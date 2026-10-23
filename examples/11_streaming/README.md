# 11：流式上传

客户端通过 `nb_proxy_stream_send` 分片上传，服务端在收到带 `NB_STREAM_LAST`
的 chunk 后一次性回复汇总结果。两个 node 在同一进程内用 TCP 直连，服务端端口
由系统自动分配。

```powershell
.\example_11_streaming.exe
```

该示例不依赖其他进程，适合观察 `NB_STREAM_FIRST` / `NB_STREAM_LAST`、
chunk offset 和流生命周期。
