cmake_minimum_required(VERSION 3.14)
project(mempool VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# By default, enable ASan/UBSan, but keep TSAN off (mutually exclusive).
option(MEMPOOL_ENABLE_SANITIZERS "Enable Address/UB sanitizers (ASan+UBSan)" ON)
option(MEMPOOL_ENABLE_TSAN       "Enable ThreadSanitizer (mutually exclusive with ASan/UBSan)" OFF)
option(MEMPOOL_ENABLE_COVERAGE   "Enable coverage instrumentation" ON)

# Common warning flags
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
endif()

# Sanitizers & coverage (host builds)
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Make ASan/UBSan and TSAN mutually exclusive; they cannot be combined.
    if (MEMPOOL_ENABLE_SANITIZERS AND MEMPOOL_ENABLE_TSAN)
        message(FATAL_ERROR
            "Cannot enable both MEMPOOL_ENABLE_SANITIZERS (ASan/UBSan) "
            "and MEMPOOL_ENABLE_TSAN (TSAN). Please enable only one.")
    endif()

    set(SAN_FLAGS "")

    if (MEMPOOL_ENABLE_TSAN)
        message(STATUS "Enabling ThreadSanitizer")
        set(SAN_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
    elseif (MEMPOOL_ENABLE_SANITIZERS)
        message(STATUS "Enabling AddressSanitizer + UndefinedBehaviorSanitizer")
        set(SAN_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    endif()

    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${SAN_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS}")

    if (MEMPOOL_ENABLE_COVERAGE)
        message(STATUS "Enabling coverage flags")
        set(COV_FLAGS "--coverage -O0")
        set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${COV_FLAGS}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COV_FLAGS}")
    endif()
endif()

# Library
add_library(mempool STATIC src/mempool.c)
target_include_directories(mempool PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# C test harness
add_executable(mempool_ctest
    src/mempool.c
    tests/test_mempool.c
)
target_include_directories(mempool_ctest PRIVATE include)
target_link_libraries(mempool_ctest mempool)

# Examples
add_executable(example_basic examples/basic_usage.c)
target_include_directories(example_basic PRIVATE include)
target_link_libraries(example_basic mempool)

add_executable(example_embedded examples/embedded_example.c)
target_include_directories(example_embedded PRIVATE include)
target_link_libraries(example_embedded mempool)

add_executable(example_stress examples/stress_test.c)
target_include_directories(example_stress PRIVATE include)
target_link_libraries(example_stress mempool)

# GoogleTest (for host builds)
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

add_test(NAME mempool_ctest COMMAND mempool_ctest)

# Installation
install(TARGETS mempool
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES include/mempool.h DESTINATION include)

# CPack packaging
set(CPACK_GENERATOR "TGZ;ZIP")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
include(CPack)
