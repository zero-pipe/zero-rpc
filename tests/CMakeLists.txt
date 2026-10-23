# Test taxonomy (also used by tools/gate.py):
#   unit = pure module / codec / config logic, no sockets
#   e2e  = end-to-end loopback / streaming with real threads and sockets
#
# Run one category:
#   ctest --test-dir build -L unit
#   ctest --test-dir build -L e2e
# Or use the unified gate:
#   python3 tools/gate.py unit
#   python3 tools/gate.py e2e

add_executable(test_framing test_framing.c)
target_include_directories(test_framing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/message)
target_link_libraries(test_framing PRIVATE rpc)
add_test(NAME unit.framing COMMAND test_framing)
set_tests_properties(unit.framing PROPERTIES LABELS "unit;framing" TIMEOUT 120)

add_executable(test_loopback test_loopback.c)
target_link_libraries(test_loopback PRIVATE rpc)
add_test(NAME e2e.loopback COMMAND test_loopback)
set_tests_properties(e2e.loopback PROPERTIES LABELS "e2e;loopback" TIMEOUT 300)

add_executable(test_config test_config.c)
target_link_libraries(test_config PRIVATE rpc)
add_test(NAME unit.config COMMAND test_config)
set_tests_properties(unit.config PROPERTIES LABELS "unit;config" TIMEOUT 120)

add_executable(test_metrics test_metrics.c)
target_include_directories(test_metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/util)
target_link_libraries(test_metrics PRIVATE rpc)
add_test(NAME unit.metrics COMMAND test_metrics)
set_tests_properties(unit.metrics PROPERTIES LABELS "unit;metrics" TIMEOUT 120)

add_executable(test_registry test_registry.c)
target_include_directories(test_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/registry)
target_link_libraries(test_registry PRIVATE rpc)
add_test(NAME unit.registry COMMAND test_registry)
set_tests_properties(unit.registry PROPERTIES LABELS "unit;registry" TIMEOUT 120)

add_executable(test_reliable test_reliable.c)
target_include_directories(test_reliable PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport/common)
target_link_libraries(test_reliable PRIVATE rpc)
add_test(NAME unit.reliable COMMAND test_reliable)
set_tests_properties(unit.reliable PROPERTIES LABELS "unit;reliable" TIMEOUT 120)

add_executable(test_stream test_stream.c)
target_link_libraries(test_stream PRIVATE rpc)
add_test(NAME e2e.stream COMMAND test_stream)
set_tests_properties(e2e.stream PROPERTIES LABELS "e2e;stream" TIMEOUT 300)

add_executable(test_pool test_pool.c)
target_include_directories(test_pool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/util
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/discovery
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/message
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/registry
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/router
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport/common
    ${CMAKE_SOURCE_DIR}/3rdparty/uthash)
target_link_libraries(test_pool PRIVATE rpc)
add_test(NAME unit.pool COMMAND test_pool)
set_tests_properties(unit.pool PROPERTIES LABELS "unit;pool" TIMEOUT 120)

add_executable(test_iobuf test_iobuf.c)
target_include_directories(test_iobuf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/util)
target_link_libraries(test_iobuf PRIVATE rpc)
add_test(NAME unit.iobuf COMMAND test_iobuf)
set_tests_properties(unit.iobuf PROPERTIES LABELS "unit;iobuf" TIMEOUT 120)

add_executable(test_api test_api.c)
target_link_libraries(test_api PRIVATE rpc)
add_test(NAME e2e.api COMMAND test_api)
set_tests_properties(e2e.api PROPERTIES LABELS "e2e;api" TIMEOUT 120)

add_executable(test_worker test_worker.c)
target_link_libraries(test_worker PRIVATE rpc)
add_test(NAME e2e.worker COMMAND test_worker)
set_tests_properties(e2e.worker PROPERTIES LABELS "e2e;worker" TIMEOUT 120)

add_executable(test_semantics test_semantics.c)
target_link_libraries(test_semantics PRIVATE rpc)
add_test(NAME e2e.semantics COMMAND test_semantics)
set_tests_properties(e2e.semantics PROPERTIES LABELS "e2e;semantics" TIMEOUT 120)

add_executable(test_headers test_headers.c)
target_link_libraries(test_headers PRIVATE rpc)
add_test(NAME unit.headers COMMAND test_headers)
set_tests_properties(unit.headers PROPERTIES LABELS "unit;headers" TIMEOUT 30)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_shm test_shm.c)
    target_link_libraries(test_shm PRIVATE rpc)
    add_test(NAME e2e.shm COMMAND test_shm)
    set_tests_properties(e2e.shm PROPERTIES LABELS "e2e;shm" TIMEOUT 120)
endif()
