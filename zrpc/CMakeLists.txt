add_library(zrpc STATIC
    src/core/node.c
    src/core/node_access.c
    src/core/transport_factory.c
    src/core/dispatch.c
    src/core/service.c
    src/core/reply.c
    src/core/proxy.c
    src/core/stub.c
    src/core/stream.c
    src/core/message_view.c
    src/core/service_table.c
    src/core/sync.c
    src/core/pending.c
    src/core/codec.c
    src/core/mem.c
    src/core/util.c
    src/core/iobuf.c
    src/core/endpoint.c
    src/message/envelope.c
    src/router/router.c
    src/registry/registry.c
    src/discovery/discovery.c
    src/config/config.c
    src/transport/framing.c
    src/transport/transport.c
    src/transport/provider.c
    src/transport/rx_window.c
    src/transport/reasm.c
    src/transport/tx_ring.c
    src/transport/udp.c
    src/transport/tcp.c
    ${CMAKE_SOURCE_DIR}/3rdpart/cjson/cJSON.c
)

target_include_directories(zrpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
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

# Keep the public layout usable from an installed package as well as from the
# build tree.  The role based headers are convenience entry points; the
# existing zrpc_*.h headers remain available for fine grained includes.
install(TARGETS zrpc
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/zrpc DESTINATION include)

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
