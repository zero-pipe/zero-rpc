add_library(zrpc STATIC
    src/core/zrpc_node.c
    src/core/zrpc_service.c
    src/core/zrpc_proxy.c
    src/core/zrpc_pending.c
    src/core/zrpc_codec.c
    src/core/zrpc_endpoint.c
    src/message/zrpc_envelope.c
    src/router/zrpc_router.c
    src/registry/zrpc_registry.c
    src/discovery/zrpc_discovery.c
    src/config/zrpc_config.c
    src/transport/zrpc_wire.c
    src/transport/zrpc_transport.c
    src/transport/zrpc_provider.c
    src/transport/zrpc_rx_window.c
    src/transport/zrpc_reasm.c
    src/transport/zrpc_tx_ring.c
    src/transport/zrpc_udp.c
    src/transport/zrpc_tcp.c
    ${CMAKE_SOURCE_DIR}/3rdpart/cjson/cJSON.c
)

target_include_directories(zrpc
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/message
        ${CMAKE_CURRENT_SOURCE_DIR}/src/transport
        ${CMAKE_CURRENT_SOURCE_DIR}/src/router
        ${CMAKE_CURRENT_SOURCE_DIR}/src/registry
        ${CMAKE_CURRENT_SOURCE_DIR}/src/discovery
        ${CMAKE_CURRENT_SOURCE_DIR}/src/config
        ${CMAKE_SOURCE_DIR}/3rdpart/uthash
        ${CMAKE_SOURCE_DIR}/3rdpart/cjson
)

target_link_libraries(zrpc PUBLIC ztk)

set_target_properties(zrpc PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME zrpc
)

if(MSVC)
    target_compile_definitions(zrpc PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

if(ZRPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ZRPC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(ZRPC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ZRPC_BUILD_GTESTS OR ZRPC_BUILD_BENCHMARKS)
    add_subdirectory(tests/google)
endif()
