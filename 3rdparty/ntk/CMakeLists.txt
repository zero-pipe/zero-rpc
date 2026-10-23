cmake_minimum_required(VERSION 3.14)
project(ntk C)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(NTK_VERSION_MAJOR 0)
set(NTK_VERSION_MINOR 8)
set(NTK_VERSION_PATCH 0)

if (NOT DEFINED NTK_BUILD_SHARED)
    option(NTK_BUILD_SHARED "Build shared library" OFF)
endif ()
if (NOT DEFINED NTK_ENABLE_EPOLL)
    option(NTK_ENABLE_EPOLL "Use epoll poller backend (Linux)" ON)
endif ()
if (NOT DEFINED NTK_ENABLE_WEPOLL)
    option(NTK_ENABLE_WEPOLL "Use wepoll poller backend (Win32)" ON)
endif ()
if (NOT DEFINED NTK_ENABLE_SELECT)
    option(NTK_ENABLE_SELECT "Use select poller backend (generic POSIX/RTOS)" ON)
endif ()
if (NOT DEFINED NTK_ALLOW_WIN32_STUB)
    option(NTK_ALLOW_WIN32_STUB "Build Win32 stub poller (no wepoll) for IDE check" OFF)
endif ()
if (NOT DEFINED NTK_ENABLE_OPENSSL)
    option(NTK_ENABLE_OPENSSL "Enable OpenSSL TLS client (ntk_tls_client)" ON)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NTKPlatform)

if (WIN32 AND NOT NTK_ALLOW_WIN32_STUB AND NOT NTK_ENABLE_WEPOLL)
    message(FATAL_ERROR
        "NTK on Windows needs -DNTK_ENABLE_WEPOLL=ON (default) or "
        "-DNTK_ALLOW_WIN32_STUB=ON. Full socket I/O still requires Linux/WSL.")
endif ()

if (NTK_ENABLE_EPOLL AND NTK_PLATFORM_LINUX)
    set(NTK_HAVE_EPOLL 1)
endif ()
if (NTK_ENABLE_WEPOLL AND NTK_PLATFORM_WIN32)
    set(NTK_HAVE_WEPOLL 1)
endif ()
if (NTK_ENABLE_SELECT AND NTK_PLATFORM_GENERIC AND NTK_HAVE_SELECT)
    set(NTK_HAVE_SELECT 1)
endif ()

find_package(Threads REQUIRED)

set(NTK_SOURCES
    src/core/ntk_errno.c
    src/core/ntk.c
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

set(NTK_HAVE_OPENSSL 0)
if (NTK_ENABLE_OPENSSL)
    if (OPENSSL_ROOT_DIR)
        set(OPENSSL_ROOT_DIR "${OPENSSL_ROOT_DIR}" CACHE PATH "OpenSSL install prefix")
    endif ()
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND AND OPENSSL_ROOT_DIR)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindOpenSSLWin.cmake")
    endif ()
    if (OpenSSL_FOUND)
        list(APPEND NTK_SOURCES src/net/ssl_ctx.c src/net/tls_client.c)
        set(NTK_HAVE_OPENSSL 1)
    else ()
        message(WARNING "NTK_ENABLE_OPENSSL=ON but OpenSSL not found; TLS disabled")
    endif ()
endif ()

configure_file(
    cmake/ntk_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ntk_config.h
    @ONLY
)

if (NTK_PLATFORM_LINUX)
    list(APPEND NTK_SOURCES
        src/platform/linux/platform_linux.c
        src/platform/linux/sync_pthread.c
        src/platform/linux/sem_pthread.c
        src/platform/linux/thread_pthread.c
        src/platform/linux/thread_priority_linux.c
        src/poller/poller_epoll.c
        src/net/linux/sock_linux.c
    )
elseif (NTK_PLATFORM_WIN32)
    list(APPEND NTK_SOURCES
        src/platform/win32/platform_win32.c
        src/platform/win32/sync_win32.c
        src/platform/win32/sem_win32.c
        src/platform/win32/thread_win32.c
        src/platform/win32/thread_priority_win32.c
        src/net/win32/sock_win32.c
    )
    if (NTK_HAVE_WEPOLL)
        list(APPEND NTK_SOURCES
            src/platform/win32/wepoll/wepoll.c
            src/poller/win32/poller_wepoll.c
        )
    else ()
        list(APPEND NTK_SOURCES src/poller/poller_stub.c)
    endif ()
elseif (NTK_PLATFORM_GENERIC)
    if (NTK_HAVE_SELECT)
        list(APPEND NTK_SOURCES src/poller/poller_select.c)
    else ()
        list(APPEND NTK_SOURCES src/poller/poller_stub.c)
    endif ()
endif ()

if (NTK_BUILD_SHARED)
    add_library(ntk SHARED ${NTK_SOURCES})
    target_compile_definitions(ntk PRIVATE NTK_EXPORTS)
else ()
    add_library(ntk STATIC ${NTK_SOURCES})
endif ()

target_include_directories(ntk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/poller
)

target_compile_definitions(ntk
    PRIVATE
        $<$<BOOL:${NTK_BUILD_SHARED}>:NTK_BUILD_SHARED>
)

if (NTK_PLATFORM_LINUX)
    target_link_libraries(ntk PUBLIC Threads::Threads)
endif ()
if (NTK_HAVE_WEPOLL)
    target_include_directories(ntk PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/wepoll"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/wepoll/sys"
    )
    target_link_libraries(ntk PUBLIC ws2_32)
endif ()
if (NTK_HAVE_OPENSSL)
    target_compile_definitions(ntk PUBLIC NTK_HAVE_OPENSSL=1)
    target_include_directories(ntk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/net")
    target_link_libraries(ntk PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif ()

set_target_properties(ntk PROPERTIES
    OUTPUT_NAME ntk
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_definitions(ntk PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NTK_HAVE_EPOLL    ${NTK_HAVE_EPOLL}    PARENT_SCOPE)
    set(NTK_HAVE_WEPOLL   ${NTK_HAVE_WEPOLL}   PARENT_SCOPE)
    set(NTK_HAVE_SELECT   ${NTK_HAVE_SELECT}    PARENT_SCOPE)
    set(NTK_HAVE_OPENSSL  ${NTK_HAVE_OPENSSL}   PARENT_SCOPE)
    set(NTK_PLATFORM_LINUX   ${NTK_PLATFORM_LINUX}   PARENT_SCOPE)
    set(NTK_PLATFORM_WIN32   ${NTK_PLATFORM_WIN32}   PARENT_SCOPE)
    set(NTK_PLATFORM_GENERIC ${NTK_PLATFORM_GENERIC} PARENT_SCOPE)
endif ()

install(TARGETS ntk
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
