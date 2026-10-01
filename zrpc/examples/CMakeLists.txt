add_executable(zrpc_echo_server echo_server.c)
target_link_libraries(zrpc_echo_server PRIVATE zrpc)

add_executable(zrpc_echo_client echo_client.c)
target_link_libraries(zrpc_echo_client PRIVATE zrpc)

if(MSVC)
    target_compile_definitions(zrpc_echo_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(zrpc_echo_client PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
