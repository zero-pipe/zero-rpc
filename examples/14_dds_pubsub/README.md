# 14：DDS Pub/Sub

使用内置 `dds` provider 演示 Cyclone DDS 的发布订阅。订阅端是普通 service，
方法名即 topic；发布端是普通 proxy，用单向事件调用发送。

```powershell
.\example_14_dds_pubsub.exe
```

绑定 URI 为 `dds://<domain>:1`，host 是 DDS domain，port 仅用于满足现有 endpoint
解析和路由，provider 会忽略它。topic 由方法名派生为 `<method>_evt`。

程序会重复发布直到订阅端收到，避免 DDS discovery 完成前丢首包。
