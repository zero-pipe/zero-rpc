# Locate OpenSSL on Windows when OPENSSL_ROOT_DIR points at the official installer tree.
if (OPENSSL_FOUND OR NOT WIN32)
    return()
endif ()
if (NOT OPENSSL_ROOT_DIR OR NOT EXISTS "${OPENSSL_ROOT_DIR}/include/openssl/ssl.h")
    return()
endif ()

set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
set(_ossl_libdir "${OPENSSL_ROOT_DIR}/lib")
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    if (EXISTS "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD/libssl.lib")
        set(_ossl_libdir "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD")
    endif ()
else ()
    if (EXISTS "${OPENSSL_ROOT_DIR}/lib/VC/x86/MD/libssl.lib")
        set(_ossl_libdir "${OPENSSL_ROOT_DIR}/lib/VC/x86/MD")
    endif ()
endif ()

find_library(OPENSSL_SSL_LIBRARY NAMES ssl libssl PATHS "${_ossl_libdir}" NO_DEFAULT_PATH)
find_library(OPENSSL_CRYPTO_LIBRARY NAMES crypto libcrypto PATHS "${_ossl_libdir}" NO_DEFAULT_PATH)

if (OPENSSL_SSL_LIBRARY AND OPENSSL_CRYPTO_LIBRARY)
    set(OpenSSL_FOUND TRUE)
    set(OPENSSL_FOUND TRUE)
    if (NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
        set_target_properties(OpenSSL::Crypto PROPERTIES
            IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
    endif ()
    if (NOT TARGET OpenSSL::SSL)
        add_library(OpenSSL::SSL UNKNOWN IMPORTED)
        set_target_properties(OpenSSL::SSL PROPERTIES
            IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
    endif ()
    message(STATUS "OpenSSL (Win64 tree): ${OPENSSL_ROOT_DIR}")
endif ()
