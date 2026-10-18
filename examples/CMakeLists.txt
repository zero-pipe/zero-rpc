# Numbered examples mirror the fastcall-c examples/ layout.
add_executable(example_01_hello_node 01_hello_node.c)
add_executable(example_02_service 02_service.c)
add_executable(example_03_proxy 03_proxy.c)
add_executable(example_04_service_table 04_service_table.c)
add_executable(example_05_sync_call 05_sync_call.c)
add_executable(example_06_static_route_server 06_static_route_server.c)
add_executable(example_06_static_route_client 06_static_route_client.c)
add_executable(example_07_direct_endpoint_server 07_direct_endpoint_server.c)
add_executable(example_07_direct_endpoint_client 07_direct_endpoint_client.c)
add_executable(example_08_config_server 08_config_server.c)
add_executable(example_08_config_client 08_config_client.c)
add_executable(example_09_service_discovery_server 09_service_discovery_server.c)
add_executable(example_09_service_discovery_client 09_service_discovery_client.c)

target_link_libraries(example_01_hello_node PRIVATE rpc)
target_link_libraries(example_02_service PRIVATE rpc)
target_link_libraries(example_03_proxy PRIVATE rpc)
target_link_libraries(example_04_service_table PRIVATE rpc)
target_link_libraries(example_05_sync_call PRIVATE rpc)
target_link_libraries(example_06_static_route_server PRIVATE rpc)
target_link_libraries(example_06_static_route_client PRIVATE rpc)
target_link_libraries(example_07_direct_endpoint_server PRIVATE rpc)
target_link_libraries(example_07_direct_endpoint_client PRIVATE rpc)
target_link_libraries(example_08_config_server PRIVATE rpc)
target_link_libraries(example_08_config_client PRIVATE rpc)
target_link_libraries(example_09_service_discovery_server PRIVATE rpc)
target_link_libraries(example_09_service_discovery_client PRIVATE rpc)

# 把配置文件拷到可执行文件旁，示例默认按同名 json 加载。
add_custom_command(TARGET example_08_config_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/08_config_server.json
        $<TARGET_FILE_DIR:example_08_config_server>/08_config_server.json)
add_custom_command(TARGET example_08_config_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/08_config_client.json
        $<TARGET_FILE_DIR:example_08_config_client>/08_config_client.json)

if(MSVC)
    target_compile_definitions(example_02_service PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(example_05_sync_call PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(example_06_static_route_client PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(example_07_direct_endpoint_client PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(example_09_service_discovery_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(example_09_service_discovery_client PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
