# SPDX-License-Identifier: MIT OR GPL-2.0-only
cmake_minimum_required(VERSION 3.25)
project(quetschn LANGUAGES CXX)

# Tools and the benchmark harness are C++20. The codec core will be C11, see PLAN.md §4.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(QUETSCHN_WERROR "Treat warnings as errors" OFF)
option(QUETSCHN_SANITIZE "Build with ASan and UBSan" OFF)

add_library(quetschn_warnings INTERFACE)
target_compile_options(quetschn_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(QUETSCHN_WERROR)
    target_compile_options(quetschn_warnings INTERFACE -Werror)
endif()
if(QUETSCHN_SANITIZE)
    target_compile_options(quetschn_warnings INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(quetschn_warnings INTERFACE -fsanitize=address,undefined)
endif()

add_library(quetschn_bench STATIC bench/zsmalloc_cost.cpp)
target_include_directories(quetschn_bench PUBLIC bench)
target_link_libraries(quetschn_bench PRIVATE quetschn_warnings)

# Page corpus collectors, see PLAN.md Phase 1. Linux only, they read /proc.
add_library(quetschn_collect STATIC tools/collect/page_stats.cpp tools/collect/resident.cpp)
target_include_directories(quetschn_collect PUBLIC tools/collect)
target_link_libraries(quetschn_collect PRIVATE quetschn_warnings)

add_executable(quetschn-collect-resident tools/collect/collect_resident_main.cpp)
target_link_libraries(quetschn-collect-resident PRIVATE quetschn_collect quetschn_warnings)

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.5.3
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(doctest)

    add_executable(quetschn_test test/main.cpp test/zsmalloc_cost_test.cpp test/page_stats_test.cpp test/resident_test.cpp)
    target_link_libraries(quetschn_test PRIVATE quetschn_bench quetschn_collect quetschn_warnings doctest::doctest)
    add_test(NAME quetschn_test COMMAND quetschn_test)
endif()
