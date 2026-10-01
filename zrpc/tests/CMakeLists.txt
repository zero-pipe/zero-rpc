add_executable(test_zrpc_wire test_zrpc_wire.c)
target_include_directories(test_zrpc_wire PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/message)
target_link_libraries(test_zrpc_wire PRIVATE zrpc)
add_test(NAME zrpc_wire COMMAND test_zrpc_wire)

add_executable(test_zrpc_loopback test_zrpc_loopback.c)
target_link_libraries(test_zrpc_loopback PRIVATE zrpc)
add_test(NAME zrpc_loopback COMMAND test_zrpc_loopback)

add_executable(test_zrpc_config test_zrpc_config.c)
target_link_libraries(test_zrpc_config PRIVATE zrpc)
add_test(NAME zrpc_config COMMAND test_zrpc_config)

add_executable(test_zrpc_registry test_zrpc_registry.c)
target_include_directories(test_zrpc_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/registry)
target_link_libraries(test_zrpc_registry PRIVATE zrpc)
add_test(NAME zrpc_registry COMMAND test_zrpc_registry)

add_executable(test_zrpc_reliable test_zrpc_reliable.c)
target_include_directories(test_zrpc_reliable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport)
target_link_libraries(test_zrpc_reliable PRIVATE zrpc)
add_test(NAME zrpc_reliable COMMAND test_zrpc_reliable)

add_executable(test_zrpc_stream test_zrpc_stream.c)
target_link_libraries(test_zrpc_stream PRIVATE zrpc)
add_test(NAME zrpc_stream COMMAND test_zrpc_stream)

# CTest labels drive gating: `ctest -L unit`, `ctest -L integration`.
# unit        = pure module / codec / config logic
# integration = end-to-end loopback / streaming with real threads + sockets
set_tests_properties(zrpc_wire zrpc_config zrpc_registry zrpc_reliable
    PROPERTIES LABELS "unit" TIMEOUT 120)
set_tests_properties(zrpc_loopback zrpc_stream
    PROPERTIES LABELS "integration" TIMEOUT 300)
