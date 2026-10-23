# 06：静态路由

服务端监听固定地址，客户端用 `nb_node_add_static_route` 登记
`OrderService -> 127.0.0.1:42420`，完全跳过服务发现。

```powershell
.\example_06_static_route_server.exe
.\example_06_static_route_client.exe
```

适合单机、测试环境或已有配置中心的部署。修改端口时需要同时改客户端和服务端。
