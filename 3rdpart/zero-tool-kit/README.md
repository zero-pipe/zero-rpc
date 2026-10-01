# Zero Tool Kit (ZTK)

## 功能特性

- **事件循环**：`ztk_poller`，支持异步任务投递、延迟定时器、负载均衡
- **网络模块**：非阻塞 socket，TcpServer/TcpClient，UdpServer/UdpClient，可选 OpenSSL TLS
- **线程模型**：`ztk_poller` / `ztk_poller_pool`（I/O 事件），`ztk_thread_pool`（阻塞任务）
- **跨平台**：Linux（epoll），Windows（wepoll + Winsock），通用 POSIX（select）

## 快速开始

**Linux / WSL：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

**Windows（MSVC）：**

```powershell
cmake -S . -B build -DZTK_ENABLE_WEPOLL=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 集成到项目

在父项目 `CMakeLists.txt` 中：

在 [zero-media-server](https://github.com/zero-pipe/zero-media-server) 中，本仓库作为子模块位于 `3rdpart/zero-tool-kit`（ZTK — 请勿与 zero-media-server 或 zero-media-kit 混淆）：

```bash
git submodule add -b master https://github.com/zero-pipe/zero-tool-kit.git 3rdpart/zero-tool-kit
```

```cmake
add_subdirectory(3rdpart/zero-tool-kit)
target_link_libraries(my_app PRIVATE ztk)
```

最小示例：

```c
#include <ztk/ztk.h>

int main(void) {
    ztk_platform_init();
    ztk_info("ZTK %s", ztk_version_string());
    return 0;
}
```

## 目录结构

```
zero-tool-kit/
  include/ztk/     对外公共头文件
  src/             实现（platform / poller / net / thread / util）
  tests/           单元与集成测试（CTest）
  examples/        可运行 demo（demo_01 … demo_05）
  docs/            设计与使用文档
  cmake/           CMake 辅助脚本
```

## CMake 选项


| 选项                   | 默认  | 说明                             |
| -------------------- | --- | ------------------------------ |
| `ZTK_BUILD_TESTS`    | ON  | 构建测试并注册 CTest                  |
| `ZTK_BUILD_EXAMPLES` | ON  | 构建 `examples/demo_`*           |
| `ZTK_BUILD_SHARED`   | OFF | 构建动态库                          |
| `ZTK_ENABLE_EPOLL`   | ON  | Linux epoll 后端                 |
| `ZTK_ENABLE_WEPOLL`  | ON  | Windows wepoll 后端              |
| `ZTK_ENABLE_SELECT`  | ON  | 通用 POSIX select 后端（RTOS / 嵌入式） |
| `ZTK_ENABLE_OPENSSL` | ON  | TLS 客户端（需要 OpenSSL）            |


