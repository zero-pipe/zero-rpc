# 01：Node 生命周期

创建并销毁一个 `nb_node_t`，不启动监听、不做网络调用。使用 STATIC 模式，
适合先确认构建和运行时依赖正常。

```powershell
.\example_01_hello_node.exe
```

预期输出包含节点名、传输类型和 binding 数量。
