# 10：Payload Codec

注册一个应用层 `nb_codec_t`，把 `order_t` 编码成 `"id|name"` 文本，再解码回来。
框架只负责透传字节和 `encoding` 标签，不解释业务格式。

```powershell
.\example_10_payload_codec.exe
```

程序会打印编码后的 payload 和解码出的订单字段。生产代码可把文本格式替换为
JSON、protobuf 或自定义 IDL。
