# 平台检测：设置 NTK_PLATFORM_* 与可选特性
if (CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    set(NTK_PLATFORM_LINUX 1)
    set(NTK_PLATFORM_NAME "linux")
elseif (WIN32)
    set(NTK_PLATFORM_WIN32 1)
    set(NTK_PLATFORM_NAME "win32")
else ()
    set(NTK_PLATFORM_GENERIC 1)
    set(NTK_PLATFORM_NAME "generic")
endif ()

if (NTK_PLATFORM_LINUX)
    set(NTK_ENABLE_EPOLL ON)
endif ()

include(CheckSymbolExists)
check_symbol_exists(pipe2 "unistd.h" NTK_HAVE_PIPE2)
check_symbol_exists(sem_init "semaphore.h" NTK_HAVE_POSIX_SEM)

if (NTK_PLATFORM_GENERIC)
    check_symbol_exists(select "sys/select.h" NTK_HAVE_SELECT)
endif ()
