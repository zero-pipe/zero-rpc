# 09：Mesh 服务发现

客户端只知道服务端 IP 和发现端口，不配置数据端口；服务端 endpoint 通过
mesh HELLO 自动发现。默认使用 TCP 数据端口 `42450`、服务端发现端口
`41953`、客户端发现端口 `41954`。

```powershell
.\example_09_service_discovery_server.exe
.\example_09_service_discovery_client.exe
```

服务端参数：`[data_port] [discovery_port] [client_ip] [client_discovery_port]`。
客户端参数：`[server_ip] [client_discovery_port] [server_discovery_port]`。
跨主机运行时请把对端 IP 和发现端口传对。
