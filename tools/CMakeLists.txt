add_executable(bench_server bench_server.c)
target_link_libraries(bench_server PRIVATE rpc)

add_executable(loadgen loadgen.c)
target_link_libraries(loadgen PRIVATE rpc)

add_executable(sync_loadgen sync_loadgen.c)
target_link_libraries(sync_loadgen PRIVATE rpc)

add_executable(bench_observability bench_observability.c)
target_link_libraries(bench_observability PRIVATE rpc)
target_include_directories(bench_observability PRIVATE ${CMAKE_SOURCE_DIR}/src/util)

if(MSVC)
    target_compile_definitions(bench_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(loadgen PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(sync_loadgen PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(bench_observability PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
