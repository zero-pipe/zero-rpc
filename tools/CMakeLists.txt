add_executable(bench_server bench_server.c)
target_link_libraries(bench_server PRIVATE rpc)

add_executable(loadgen loadgen.c)
target_link_libraries(loadgen PRIVATE rpc)

if(MSVC)
    target_compile_definitions(bench_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(loadgen PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
