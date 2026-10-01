cmake_minimum_required(VERSION 3.14)
project(ztk C)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(ZTK_VERSION_MAJOR 0)
set(ZTK_VERSION_MINOR 8)
set(ZTK_VERSION_PATCH 0)

if (NOT DEFINED ZTK_BUILD_TESTS)
    option(ZTK_BUILD_TESTS "Build ztk unit tests" ON)
endif ()
if (NOT DEFINED ZTK_BUILD_EXAMPLES)
    option(ZTK_BUILD_EXAMPLES "Build ztk demo executables" ON)
endif ()
if (NOT DEFINED ZTK_BUILD_SHARED)
    option(ZTK_BUILD_SHARED "Build shared library" OFF)
endif ()
if (NOT DEFINED ZTK_ENABLE_EPOLL)
    option(ZTK_ENABLE_EPOLL "Use epoll poller backend (Linux)" ON)
endif ()
if (NOT DEFINED ZTK_ENABLE_WEPOLL)
    option(ZTK_ENABLE_WEPOLL "Use wepoll poller backend (Win32)" ON)
endif ()
if (NOT DEFINED ZTK_ENABLE_SELECT)
    option(ZTK_ENABLE_SELECT "Use select poller backend (generic POSIX/RTOS)" ON)
endif ()
if (NOT DEFINED ZTK_ALLOW_WIN32_STUB)
    option(ZTK_ALLOW_WIN32_STUB "Build Win32 stub poller (no wepoll) for IDE check" OFF)
endif ()
if (NOT DEFINED ZTK_ENABLE_OPENSSL)
    option(ZTK_ENABLE_OPENSSL "Enable OpenSSL TLS client (ztk_tls_client)" ON)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ZTKPlatform)

if (WIN32 AND NOT ZTK_ALLOW_WIN32_STUB AND NOT ZTK_ENABLE_WEPOLL)
    message(FATAL_ERROR
        "ZTK on Windows needs -DZTK_ENABLE_WEPOLL=ON (default) or "
        "-DZTK_ALLOW_WIN32_STUB=ON. Full socket I/O still requires Linux/WSL.")
endif ()

if (ZTK_ENABLE_EPOLL AND ZTK_PLATFORM_LINUX)
    set(ZTK_HAVE_EPOLL 1)
endif ()
if (ZTK_ENABLE_WEPOLL AND ZTK_PLATFORM_WIN32)
    set(ZTK_HAVE_WEPOLL 1)
endif ()
if (ZTK_ENABLE_SELECT AND ZTK_PLATFORM_GENERIC AND ZTK_HAVE_SELECT)
    set(ZTK_HAVE_SELECT 1)
endif ()

find_package(Threads REQUIRED)

set(ZTK_SOURCES
    src/core/ztk_errno.c
    src/core/ztk.c
    src/platform/platform.c
    src/util/log.c
    src/util/byte_buf.c
    src/util/buf.c
    src/util/mpsc.c
    src/util/timer.c
    src/net/socket.c
    src/net/tcp_server.c
    src/net/tcp_client.c
    src/net/udp_server.c
    src/net/udp_client.c
    src/net/rtp_port.c
    src/poller/poller_task.c
    src/poller/poller_timer.c
    src/poller/poller_load.c
    src/poller/poller_pool.c
    src/poller/poller_buf.c
    src/thread/thread_pool.c
)

set(ZTK_HAVE_OPENSSL 0)
if (ZTK_ENABLE_OPENSSL)
    if (OPENSSL_ROOT_DIR)
        set(OPENSSL_ROOT_DIR "${OPENSSL_ROOT_DIR}" CACHE PATH "OpenSSL install prefix")
    endif ()
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND AND OPENSSL_ROOT_DIR)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindOpenSSLWin.cmake")
    endif ()
    if (OpenSSL_FOUND)
        list(APPEND ZTK_SOURCES src/net/ssl_ctx.c src/net/tls_client.c)
        set(ZTK_HAVE_OPENSSL 1)
    else ()
        message(WARNING "ZTK_ENABLE_OPENSSL=ON but OpenSSL not found; TLS disabled")
    endif ()
endif ()

configure_file(
    cmake/ztk_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ztk_config.h
    @ONLY
)

if (ZTK_PLATFORM_LINUX)
    list(APPEND ZTK_SOURCES
        src/platform/linux/platform_linux.c
        src/platform/linux/sync_pthread.c
        src/platform/linux/sem_pthread.c
        src/platform/linux/thread_pthread.c
        src/platform/linux/thread_priority_linux.c
        src/poller/poller_epoll.c
        src/net/linux/sock_linux.c
    )
elseif (ZTK_PLATFORM_WIN32)
    list(APPEND ZTK_SOURCES
        src/platform/win32/platform_win32.c
        src/platform/win32/sync_win32.c
        src/platform/win32/sem_win32.c
        src/platform/win32/thread_win32.c
        src/platform/win32/thread_priority_win32.c
        src/net/win32/sock_win32.c
    )
    if (ZTK_HAVE_WEPOLL)
        list(APPEND ZTK_SOURCES
            src/platform/win32/wepoll/wepoll.c
            src/poller/win32/poller_wepoll.c
        )
    else ()
        list(APPEND ZTK_SOURCES src/poller/poller_stub.c)
    endif ()
elseif (ZTK_PLATFORM_GENERIC)
    if (ZTK_HAVE_SELECT)
        list(APPEND ZTK_SOURCES src/poller/poller_select.c)
    else ()
        list(APPEND ZTK_SOURCES src/poller/poller_stub.c)
    endif ()
endif ()

if (ZTK_BUILD_SHARED)
    add_library(ztk SHARED ${ZTK_SOURCES})
    target_compile_definitions(ztk PRIVATE ZTK_EXPORTS)
else ()
    add_library(ztk STATIC ${ZTK_SOURCES})
endif ()

target_include_directories(ztk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/poller
)

target_compile_definitions(ztk
    PRIVATE
        $<$<BOOL:${ZTK_BUILD_SHARED}>:ZTK_BUILD_SHARED>
)

if (ZTK_PLATFORM_LINUX)
    target_link_libraries(ztk PUBLIC Threads::Threads)
endif ()
if (ZTK_HAVE_WEPOLL)
    target_include_directories(ztk PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/wepoll"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/wepoll/sys"
    )
    target_link_libraries(ztk PUBLIC ws2_32)
endif ()
if (ZTK_HAVE_OPENSSL)
    target_compile_definitions(ztk PUBLIC ZTK_HAVE_OPENSSL=1)
    target_include_directories(ztk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/net")
    target_link_libraries(ztk PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif ()

set_target_properties(ztk PROPERTIES
    OUTPUT_NAME ztk
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_definitions(ztk PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ZTK_HAVE_EPOLL    ${ZTK_HAVE_EPOLL}    PARENT_SCOPE)
    set(ZTK_HAVE_WEPOLL   ${ZTK_HAVE_WEPOLL}   PARENT_SCOPE)
    set(ZTK_HAVE_SELECT   ${ZTK_HAVE_SELECT}    PARENT_SCOPE)
    set(ZTK_HAVE_OPENSSL  ${ZTK_HAVE_OPENSSL}   PARENT_SCOPE)
    set(ZTK_PLATFORM_LINUX   ${ZTK_PLATFORM_LINUX}   PARENT_SCOPE)
    set(ZTK_PLATFORM_WIN32   ${ZTK_PLATFORM_WIN32}   PARENT_SCOPE)
    set(ZTK_PLATFORM_GENERIC ${ZTK_PLATFORM_GENERIC} PARENT_SCOPE)
endif ()

if (ZTK_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

if (ZTK_BUILD_EXAMPLES AND (ZTK_HAVE_EPOLL OR ZTK_HAVE_WEPOLL))
    add_subdirectory(examples)
endif ()

install(TARGETS ztk
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ztk DESTINATION include)
