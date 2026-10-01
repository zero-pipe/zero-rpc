add_executable(zrpc_bench_server zrpc_bench_server.c)
target_link_libraries(zrpc_bench_server PRIVATE zrpc)

add_executable(zrpc_loadgen zrpc_loadgen.c)
target_link_libraries(zrpc_loadgen PRIVATE zrpc)

if(MSVC)
    target_compile_definitions(zrpc_bench_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(zrpc_loadgen PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
