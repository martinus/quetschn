# SPDX-License-Identifier: MIT OR GPL-2.0-only
cmake_minimum_required(VERSION 3.25)
project(quetschn LANGUAGES C CXX)

# The page size of the explore/ codecs: 12 for 4 KiB, 14 for 16 KiB. The tests assume 4 KiB.
set(QUETSCHN_PAGE_BITS 12 CACHE STRING "log2 of the page size of the explore/ codecs")
if(NOT QUETSCHN_PAGE_BITS EQUAL 12)
    add_compile_definitions(QUETSCHN_PAGE_BITS=${QUETSCHN_PAGE_BITS})
endif()


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

# Page corpus collectors, see PLAN.md Phase 1. Linux only, they read /proc.
add_library(quetschn_collect STATIC tools/collect/page_stats.cpp tools/collect/resident.cpp tools/collect/split.cpp
                                   tools/collect/import_raw.cpp)
target_include_directories(quetschn_collect PUBLIC tools/collect)
target_link_libraries(quetschn_collect PRIVATE quetschn_warnings)

add_executable(quetschn-collect-resident tools/collect/collect_resident_main.cpp)
target_link_libraries(quetschn-collect-resident PRIVATE quetschn_collect quetschn_warnings)

add_executable(quetschn-split-corpus tools/collect/split_main.cpp)
target_link_libraries(quetschn-split-corpus PRIVATE quetschn_collect quetschn_warnings)

add_executable(quetschn-sample-corpus tools/collect/sample_main.cpp)
target_link_libraries(quetschn-sample-corpus PRIVATE quetschn_collect quetschn_warnings)

add_executable(quetschn-import-raw tools/collect/import_raw_main.cpp)
target_link_libraries(quetschn-import-raw PRIVATE quetschn_collect quetschn_warnings)

add_library(quetschn_bench STATIC bench/zsmalloc_cost.cpp bench/harness.cpp bench/compare.cpp bench/score.cpp)
target_include_directories(quetschn_bench PUBLIC bench)
find_package(Threads REQUIRED)
target_link_libraries(quetschn_bench PUBLIC quetschn_collect Threads::Threads PRIVATE quetschn_warnings)

add_executable(quetschn-compare bench/compare_main.cpp)
target_link_libraries(quetschn-compare PRIVATE quetschn_bench quetschn_warnings)

add_executable(quetschn-score bench/score_main.cpp)
target_link_libraries(quetschn-score PRIVATE quetschn_bench quetschn_warnings)

add_executable(quetschn-swap-bursts tools/swap-bursts.c)
target_link_libraries(quetschn-swap-bursts PRIVATE quetschn_warnings)

# Phase 2b decoder spike, see spike/wk64.h. Built here with our warnings for the tests, and in
# cmake/kernel_codecs.cmake a second time with the kernel's flags for the benchmark.
add_library(quetschn_spike STATIC spike/wk64.c spike/zram_spike.c)
target_include_directories(quetschn_spike PUBLIC spike bench/kernel_codecs)
target_link_libraries(quetschn_spike PRIVATE quetschn_warnings)

# Candidate designs, see docs/explored-designs.md. The parts that need no kernel code, for the tests.
# seqlz's sources, for every target that builds it: the explore library, the kernel codecs, the fuzzers
set(QUETSCHN_SEQLZ_SOURCES explore/seqlz.c explore/seqlz_default_tables.c explore/seqlz_lit_sets.c)
add_library(quetschn_explore STATIC explore/shuffle.c explore/bdelta.c ${QUETSCHN_SEQLZ_SOURCES} explore/bytelz.c)
target_include_directories(quetschn_explore PUBLIC explore)
target_link_libraries(quetschn_explore PRIVATE quetschn_warnings)

# The fuzz targets of fuzz/, in libFuzzer's interface: built with clang they are libFuzzer binaries,
# built with AFL++'s afl-clang-lto as CC they run under afl-fuzz. seqlz itself gets the coverage
# instrumentation too, in a library of its own; make_seeds uses the plain one.
option(QUETSCHN_FUZZ "Build the fuzz targets of fuzz/, needs clang or afl-clang-lto" OFF)
if(QUETSCHN_FUZZ)
    add_library(quetschn_fuzz_seqlz STATIC ${QUETSCHN_SEQLZ_SOURCES})
    target_include_directories(quetschn_fuzz_seqlz PUBLIC explore)
    # afl-clang-lto instruments every file anyway, and with this flag it would need libFuzzer's runtime
    if(NOT CMAKE_C_COMPILER MATCHES "afl-")
        target_compile_options(quetschn_fuzz_seqlz PRIVATE -fsanitize=fuzzer-no-link)
    endif()
    target_link_libraries(quetschn_fuzz_seqlz PRIVATE quetschn_warnings)
    foreach(target seqlz_decode_fuzz seqlz_roundtrip_fuzz seqlz_diff_fuzz)
        add_executable(${target} fuzz/${target}.c)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_libraries(${target} PRIVATE quetschn_fuzz_seqlz quetschn_warnings)
    endforeach()
    # the decoder written from FORMAT.md, instrumented like the target
    target_sources(seqlz_diff_fuzz PRIVATE tools/seqlz_ref.c)
    target_include_directories(seqlz_diff_fuzz PRIVATE tools)
    add_executable(quetschn-fuzz-seeds fuzz/make_seeds.c)
    target_link_libraries(quetschn-fuzz-seeds PRIVATE quetschn_explore quetschn_warnings)
endif()

include(cmake/kernel_codecs.cmake)

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.5.3
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(doctest)

    add_executable(quetschn_test test/main.cpp test/zsmalloc_cost_test.cpp test/page_stats_test.cpp test/resident_test.cpp
                                 test/harness_test.cpp test/split_test.cpp test/import_raw_test.cpp
                                 test/compare_test.cpp test/score_test.cpp test/spike_test.cpp
                                 test/explore_test.cpp test/seqlz_ref_test.cpp tools/seqlz_ref.c)
    target_link_libraries(quetschn_test PRIVATE quetschn_bench quetschn_collect quetschn_spike quetschn_explore quetschn_warnings
                                                doctest::doctest)
    target_include_directories(quetschn_test PRIVATE tools)
    if(QUETSCHN_HAVE_KERNEL_CODECS)
        target_sources(quetschn_test PRIVATE test/kernel_codecs_test.cpp test/lz_analysis_test.cpp bench/lz_analysis.cpp)
        target_link_libraries(quetschn_test PRIVATE quetschn_kernel_lz4 quetschn_kernel_lzo quetschn_kernel_zstd
                                                    quetschn_kernel_explore quetschn_kernel_explore_zstd
                                                    quetschn_kernel_seqlz)
        target_compile_definitions(quetschn_test PRIVATE QUETSCHN_HAVE_KERNEL_CODECS)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            add_test(NAME kernel_seqlz_prefetch
                     COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP}
                             "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:quetschn_kernel_seqlz>,|>"
                             -P ${CMAKE_SOURCE_DIR}/cmake/check_prefetch.cmake)
        endif()
    endif()
    add_test(NAME quetschn_test COMMAND quetschn_test)
endif()
