# 03：Proxy 生命周期

只创建 node 和 proxy，展示客户端对象的创建与释放顺序。proxy 内部建立在
stub 之上，不持有独立线程。

```powershell
.\example_03_proxy.exe
```

程序创建后立即销毁，不发送请求。
