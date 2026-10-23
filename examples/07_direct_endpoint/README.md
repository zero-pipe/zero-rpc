# 07：直连端点

客户端用 `nb_proxy_set_endpoint_uri` 直接指定目标地址，跳过服务发现和路由表。

```powershell
.\example_07_direct_endpoint_server.exe
.\example_07_direct_endpoint_client.exe
```

默认地址为 `tcp://127.0.0.1:42430`。与静态路由的区别是：直连端点覆盖的是
proxy 的目标，而不是在 node 中登记服务名。
