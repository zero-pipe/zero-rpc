# 04：Service Table

用 `NB_SERVICE_DEFINE` 声明服务和方法 ID，安装后按 `service.method` 查询
数字 ID。真实项目中这张表应放在客户端和服务端共享的头文件里。

```powershell
.\example_04_service_table.exe
```

预期输出 `OrderService.get` 和 `OrderService.put` 的 ID。
