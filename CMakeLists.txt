cmake_minimum_required(VERSION 3.14)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(zero-rpc C)

if(MSVC)
    add_compile_options(/utf-8)
    # Bundled Windows GoogleTest archives use the static MSVC runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 基础依赖：zero-tool-kit
set(ZTK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZTK_ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdpart/zero-tool-kit)

# zrpc 本体与测试
option(ZRPC_BUILD_TESTS "Build zrpc tests" ON)
option(ZRPC_BUILD_TOOLS "Build zrpc benchmark tools" ON)
option(ZRPC_BUILD_EXAMPLES "Build zrpc examples" ON)
option(ZRPC_BUILD_GTESTS "Build GoogleTest module tests" OFF)
option(ZRPC_BUILD_BENCHMARKS "Build Google Benchmark microbenchmarks" OFF)
if(ZRPC_BUILD_GTESTS OR ZRPC_BUILD_BENCHMARKS)
    enable_language(CXX)
endif()
add_subdirectory(zrpc)
