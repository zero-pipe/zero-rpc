cmake_minimum_required(VERSION 3.14)
project(elog C)

add_library(elog STATIC
    easylogger/src/elog.c
    easylogger/src/elog_utils.c
    easylogger/port/elog_port.c
)

target_include_directories(elog
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/easylogger/inc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/easylogger/src
        ${CMAKE_CURRENT_SOURCE_DIR}/easylogger/port
)

set_target_properties(elog PROPERTIES
    OUTPUT_NAME elog
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if (MSVC)
    target_compile_definitions(elog PRIVATE _CRT_SECURE_NO_WARNINGS)
endif ()

if (UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(elog PUBLIC Threads::Threads)
endif ()
