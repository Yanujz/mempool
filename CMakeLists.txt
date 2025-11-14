cmake_minimum_required(VERSION 3.14)
project(mempool VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic)

# Library
add_library(mempool STATIC src/mempool.c)
target_include_directories(mempool PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Minimal C test harness
add_executable(mempool_ctest
    src/mempool.c
    tests/test_mempool.c
)
target_include_directories(mempool_ctest PRIVATE include)
target_link_libraries(mempool_ctest mempool)

# Examples (C only)
add_executable(example_basic examples/basic_usage.c)
target_include_directories(example_basic PRIVATE include)
target_link_libraries(example_basic mempool)

add_executable(example_embedded examples/embedded_example.c)
target_include_directories(example_embedded PRIVATE include)
target_link_libraries(example_embedded mempool)

add_executable(example_stress examples/stress_test.c)
target_include_directories(example_stress PRIVATE include)
target_link_libraries(example_stress mempool)

# GoogleTest-based test suite (C++)
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(mempool_gtest
    tests/gtest_mempool.cpp
)
target_include_directories(mempool_gtest PRIVATE include)
target_link_libraries(mempool_gtest
    PRIVATE
        mempool
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mempool_gtest)

# Legacy C harness as an additional test
add_test(NAME mempool_ctest COMMAND mempool_ctest)

# Installation
install(TARGETS mempool
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES include/mempool.h DESTINATION include)

# CPack for packaging
set(CPACK_GENERATOR "TGZ;ZIP")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
include(CPack)
