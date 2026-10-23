# 08：JSON 配置

从 JSON 文件创建节点。服务端监听 `tcp://127.0.0.1:42440`，客户端通过
`routes` 静态路由调用 `EchoService.echo`。

构建后两个 `.json` 文件会复制到可执行文件旁。先启动服务端，再运行客户端：

```powershell
.\example_08_config_server.exe [server.json]
.\example_08_config_client.exe [client.json]
```

不传参数时分别读取 `server.json` 和 `client.json`。字段说明见
[配置参考](../../docs/api/config.md)。
