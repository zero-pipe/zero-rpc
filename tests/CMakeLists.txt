add_executable(test_framing test_framing.c)
target_include_directories(test_framing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/message)
target_link_libraries(test_framing PRIVATE rpc)
add_test(NAME framing COMMAND test_framing)

add_executable(test_loopback test_loopback.c)
target_link_libraries(test_loopback PRIVATE rpc)
add_test(NAME loopback COMMAND test_loopback)

add_executable(test_config test_config.c)
target_link_libraries(test_config PRIVATE rpc)
add_test(NAME config COMMAND test_config)

add_executable(test_registry test_registry.c)
target_include_directories(test_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/registry)
target_link_libraries(test_registry PRIVATE rpc)
add_test(NAME registry COMMAND test_registry)

add_executable(test_reliable test_reliable.c)
target_include_directories(test_reliable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport)
target_link_libraries(test_reliable PRIVATE rpc)
add_test(NAME reliable COMMAND test_reliable)

add_executable(test_stream test_stream.c)
target_link_libraries(test_stream PRIVATE rpc)
add_test(NAME stream COMMAND test_stream)

add_executable(test_pool test_pool.c)
target_include_directories(test_pool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/discovery
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/message
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/registry
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/router
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport
    ${CMAKE_SOURCE_DIR}/3rdpart/uthash)
target_link_libraries(test_pool PRIVATE rpc)
add_test(NAME pool COMMAND test_pool)

add_executable(test_iobuf test_iobuf.c)
target_include_directories(test_iobuf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)
target_link_libraries(test_iobuf PRIVATE rpc)
add_test(NAME iobuf COMMAND test_iobuf)

add_executable(test_api test_api.c)
target_link_libraries(test_api PRIVATE rpc)
add_test(NAME api COMMAND test_api)

add_executable(test_worker test_worker.c)
target_link_libraries(test_worker PRIVATE rpc)
add_test(NAME worker COMMAND test_worker)

add_executable(test_headers test_headers.c)
target_link_libraries(test_headers PRIVATE rpc)
add_test(NAME headers COMMAND test_headers)

# CTest labels drive gating: `ctest -L unit`, `ctest -L integration`.
# unit        = pure module / codec / config logic
# integration = end-to-end loopback / streaming with real threads + sockets
set_tests_properties(framing config registry reliable pool iobuf
    PROPERTIES LABELS "unit" TIMEOUT 120)
set_tests_properties(loopback stream
    PROPERTIES LABELS "integration" TIMEOUT 300)
set_tests_properties(api worker PROPERTIES LABELS "integration" TIMEOUT 120)
set_tests_properties(headers PROPERTIES LABELS "unit" TIMEOUT 30)
