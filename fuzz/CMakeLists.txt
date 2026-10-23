if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "BUILD_FUZZERS requires Clang (libFuzzer)")
endif()

add_executable(fuzz_envelope fuzz_envelope.c)
target_include_directories(fuzz_envelope PRIVATE
    ${CMAKE_SOURCE_DIR}/src/transport/common
    ${CMAKE_SOURCE_DIR}/src/message
    ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(fuzz_envelope PRIVATE rpc)
target_compile_options(fuzz_envelope PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_envelope PRIVATE -fsanitize=fuzzer,address,undefined)

add_executable(fuzz_metadata fuzz_metadata.c)
target_link_libraries(fuzz_metadata PRIVATE rpc)
target_compile_options(fuzz_metadata PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_metadata PRIVATE -fsanitize=fuzzer,address,undefined)

add_executable(fuzz_framing fuzz_framing.c)
target_include_directories(fuzz_framing PRIVATE
    ${CMAKE_SOURCE_DIR}/src/transport/common
    ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(fuzz_framing PRIVATE rpc)
target_compile_options(fuzz_framing PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_framing PRIVATE -fsanitize=fuzzer,address,undefined)

# Smoke-run a fixed corpus for a short time when ctest is invoked in fuzz builds.
add_test(NAME fuzz.envelope COMMAND fuzz_envelope -runs=2000)
add_test(NAME fuzz.metadata COMMAND fuzz_metadata -runs=2000)
add_test(NAME fuzz.framing COMMAND fuzz_framing -runs=2000)
set_tests_properties(fuzz.envelope fuzz.metadata fuzz.framing
    PROPERTIES LABELS "fuzz" TIMEOUT 120)
