# Each example is a self-contained directory with its own CMakeLists.txt.

function(nb_add_example target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE rpc)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

function(nb_copy_example_file target file)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${file}
            $<TARGET_FILE_DIR:${target}>/${file})
endfunction()

add_subdirectory(01_hello_node)
add_subdirectory(02_service)
add_subdirectory(03_proxy)
add_subdirectory(04_service_table)
add_subdirectory(05_sync_call)
add_subdirectory(06_static_route)
add_subdirectory(07_direct_endpoint)
add_subdirectory(08_config)
add_subdirectory(09_service_discovery)
add_subdirectory(10_payload_codec)
add_subdirectory(11_streaming)
add_subdirectory(12_worker)
add_subdirectory(13_metadata_cancel)
add_subdirectory(14_dds_pubsub)

set(NB_EXAMPLE_TARGETS
    example_01_hello_node
    example_02_service
    example_03_proxy
    example_04_service_table
    example_05_sync_call
    example_06_static_route_server
    example_06_static_route_client
    example_07_direct_endpoint_server
    example_07_direct_endpoint_client
    example_08_config_server
    example_08_config_client
    example_09_service_discovery_server
    example_09_service_discovery_client
    example_10_payload_codec
    example_11_streaming
    example_12_worker
    example_13_metadata_cancel
)
if(NB_ENABLE_DDS)
    list(APPEND NB_EXAMPLE_TARGETS example_14_dds_pubsub)
endif()
install(TARGETS ${NB_EXAMPLE_TARGETS} RUNTIME DESTINATION bin)

install(FILES
    08_config/server.json
    08_config/client.json
    DESTINATION bin
)
