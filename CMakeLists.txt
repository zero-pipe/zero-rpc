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

# 基础依赖：network-tool-kit
set(ZTK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZTK_ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdpart/ntk)

option(BUILD_TESTS "Build rpc tests" ON)
option(BUILD_TOOLS "Build rpc benchmark tools" ON)
option(BUILD_EXAMPLES "Build rpc examples" ON)
option(BUILD_GTESTS "Build GoogleTest module tests" OFF)
option(BUILD_BENCHMARKS "Build Google Benchmark microbenchmarks" OFF)
if(BUILD_GTESTS OR BUILD_BENCHMARKS)
    enable_language(CXX)
endif()

add_subdirectory(src)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_GTESTS OR BUILD_BENCHMARKS)
    add_subdirectory(tests/google)
endif()
