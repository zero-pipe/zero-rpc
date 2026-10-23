cmake_minimum_required(VERSION 3.14)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(network_bridge C)

if(MSVC)
    add_compile_options(/utf-8)
    # Bundled Windows GoogleTest archives use the static MSVC runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 基础依赖：network-tool-kit
set(NTK_ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/ntk)
add_subdirectory(3rdparty/elog)

option(BUILD_TESTS "Build rpc tests" ON)
option(BUILD_TOOLS "Build rpc benchmark tools" ON)
option(BUILD_EXAMPLES "Build rpc examples" ON)
option(BUILD_GTESTS "Build GoogleTest module tests" OFF)
option(BUILD_BENCHMARKS "Build Google Benchmark microbenchmarks" OFF)
option(BUILD_FUZZERS "Build libFuzzer targets (requires Clang)" OFF)
option(NB_ENABLE_DDS "Enable the Cyclone DDS transport provider" ON)
set(ZERO_RPC_SANITIZE "" CACHE STRING "Sanitizer set: address|undefined|thread|address,undefined")

if(NB_ENABLE_DDS)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NB_CYCLONEDDS_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/cyclonedds/windows_amd64")
        set(NB_CYCLONEDDS_LIB "${NB_CYCLONEDDS_ROOT}/lib/ddsc.lib")
        set(NB_CYCLONEDDS_DLL "${NB_CYCLONEDDS_ROOT}/bin/ddsc.dll")
    elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(NB_CYCLONEDDS_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/cyclonedds/linux_arm64")
        set(NB_CYCLONEDDS_LIB "${NB_CYCLONEDDS_ROOT}/lib/libddsc.so")
    elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "loongarch64")
        set(NB_CYCLONEDDS_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/cyclonedds/linux_loongarch64")
        set(NB_CYCLONEDDS_LIB "${NB_CYCLONEDDS_ROOT}/lib/libddsc.so")
    elseif(UNIX)
        set(NB_CYCLONEDDS_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/cyclonedds/linux_amd64")
        set(NB_CYCLONEDDS_LIB "${NB_CYCLONEDDS_ROOT}/lib/libddsc.so")
    endif()

    if(NB_CYCLONEDDS_ROOT AND EXISTS "${NB_CYCLONEDDS_ROOT}/include/dds/dds.h")
        add_library(nb_cyclonedds INTERFACE)
        target_include_directories(nb_cyclonedds INTERFACE "${NB_CYCLONEDDS_ROOT}/include")
        target_link_libraries(nb_cyclonedds INTERFACE "${NB_CYCLONEDDS_LIB}")
        target_compile_definitions(nb_cyclonedds INTERFACE NB_ENABLE_DDS=1)
    else()
        message(WARNING "Cyclone DDS SDK not found; disabling NB_ENABLE_DDS")
        set(NB_ENABLE_DDS OFF CACHE BOOL "" FORCE)
    endif()
endif()

if(ZERO_RPC_SANITIZE)
    add_compile_options(-fsanitize=${ZERO_RPC_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${ZERO_RPC_SANITIZE})
endif()

if(BUILD_GTESTS OR BUILD_BENCHMARKS)
    enable_language(CXX)
endif()

add_subdirectory(src)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(BUILD_FUZZERS)
    add_subdirectory(fuzz)
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

# Unified quality gates. Build the target first, then the gate runs:
#   cmake --build build --target gate-unit
#   cmake --build build --target gate-e2e
#   cmake --build build --target gate-perf
#   cmake --build build --target gate-stability
#   cmake --build build --target gate-all
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(NB_GATE_SCRIPT ${CMAKE_SOURCE_DIR}/tools/gate.py)
    set(NB_GATE_ARGS --build-dir ${CMAKE_BINARY_DIR} --no-build)
    if(CMAKE_CONFIGURATION_TYPES)
        list(APPEND NB_GATE_ARGS --config $<CONFIG>)
    endif()

    if(TARGET test_framing)
        add_custom_target(gate-unit
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} unit ${NB_GATE_ARGS}
            DEPENDS test_framing test_config test_metrics test_registry
                    test_reliable test_pool test_iobuf test_headers
            USES_TERMINAL VERBATIM)
    endif()

    if(TARGET bench_server AND TARGET loadgen AND TARGET test_loopback)
        add_custom_target(gate-e2e
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} e2e ${NB_GATE_ARGS}
            DEPENDS test_loopback test_stream test_api test_worker test_semantics
                    bench_server loadgen
            USES_TERMINAL VERBATIM)
    endif()

    if(TARGET bench_server AND TARGET loadgen)
        add_custom_target(gate-perf
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} perf ${NB_GATE_ARGS}
            DEPENDS bench_server loadgen
            USES_TERMINAL VERBATIM)
        add_custom_target(gate-stability
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} stability ${NB_GATE_ARGS}
            DEPENDS bench_server loadgen
            USES_TERMINAL VERBATIM)
    endif()

    if(TARGET fuzz_envelope)
        add_custom_target(gate-fuzz
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} fuzz ${NB_GATE_ARGS}
            DEPENDS fuzz_envelope fuzz_metadata fuzz_framing
            USES_TERMINAL VERBATIM)
    endif()

    if(TARGET gate-unit AND TARGET gate-e2e AND TARGET gate-perf AND TARGET gate-stability)
        add_custom_target(gate-all
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} all ${NB_GATE_ARGS}
            DEPENDS test_framing test_config test_metrics test_registry
                    test_reliable test_pool test_iobuf test_headers
                    test_loopback test_stream test_api test_worker test_semantics
                    bench_server loadgen
            USES_TERMINAL VERBATIM)
    endif()
endif()
