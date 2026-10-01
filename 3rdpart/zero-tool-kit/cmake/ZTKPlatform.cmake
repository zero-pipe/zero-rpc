# 平台检测：设置 ZTK_PLATFORM_* 与可选特性
if (CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    set(ZTK_PLATFORM_LINUX 1)
    set(ZTK_PLATFORM_NAME "linux")
elseif (WIN32)
    set(ZTK_PLATFORM_WIN32 1)
    set(ZTK_PLATFORM_NAME "win32")
else ()
    set(ZTK_PLATFORM_GENERIC 1)
    set(ZTK_PLATFORM_NAME "generic")
endif ()

if (ZTK_PLATFORM_LINUX)
    set(ZTK_ENABLE_EPOLL ON)
endif ()

include(CheckSymbolExists)
check_symbol_exists(pipe2 "unistd.h" ZTK_HAVE_PIPE2)
check_symbol_exists(sem_init "semaphore.h" ZTK_HAVE_POSIX_SEM)

if (ZTK_PLATFORM_GENERIC)
    check_symbol_exists(select "sys/select.h" ZTK_HAVE_SELECT)
endif ()
