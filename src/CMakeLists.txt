add_library(rpc STATIC
    core/node.c
    core/node_access.c
    core/transport_factory.c
    core/dispatch.c
    core/service.c
    core/reply.c
    core/proxy.c
    core/stub.c
    core/stream.c
    core/message_view.c
    core/metadata.c
    core/service_table.c
    core/sync.c
    core/pending.c
    core/dedup.c
    core/codec.c
    core/log.c
    core/mem.c
    core/util.c
    core/iobuf.c
    core/endpoint.c
    message/envelope.c
    router/router.c
    registry/registry.c
    discovery/discovery.c
    config/config.c
    transport/framing.c
    transport/transport.c
    transport/provider.c
    transport/rx_window.c
    transport/reasm.c
    transport/tx_ring.c
    transport/udp.c
    transport/tcp.c
    ${CMAKE_SOURCE_DIR}/3rdpart/cjson/cJSON.c
)

target_include_directories(rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/message
        ${CMAKE_CURRENT_SOURCE_DIR}/transport
        ${CMAKE_CURRENT_SOURCE_DIR}/router
        ${CMAKE_CURRENT_SOURCE_DIR}/registry
        ${CMAKE_CURRENT_SOURCE_DIR}/discovery
        ${CMAKE_CURRENT_SOURCE_DIR}/config
        ${CMAKE_SOURCE_DIR}/3rdpart/uthash
        ${CMAKE_SOURCE_DIR}/3rdpart/cjson
)

install(TARGETS rpc
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ DESTINATION include)

target_link_libraries(rpc PUBLIC ntk elog)

set_target_properties(rpc PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME rpc
)

if(MSVC)
    target_compile_definitions(rpc PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
