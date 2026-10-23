# SPDX-License-Identifier: MIT OR GPL-2.0-only
cmake_minimum_required(VERSION 3.25)
project(seqlz LANGUAGES C CXX)

# Without a build type CMake does not optimize, and the trainer and the benchmarks run several times
# slower than the numbers in the docs. Release unless a build type is given.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

# The page size of seqlz: 12 for 4 KiB, 14 for 16 KiB. The tests of the kernel codecs need 4 KiB, the
# others run with both.
set(SEQLZ_PAGE_BITS 12 CACHE STRING "log2 of the page size of seqlz")
if(NOT SEQLZ_PAGE_BITS EQUAL 12)
    add_compile_definitions(SEQLZ_PAGE_BITS=${SEQLZ_PAGE_BITS})
endif()


# Tools and the benchmark harness are C++20. The codec core will be C11, see docs/plan.md §4.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SEQLZ_WERROR "Treat warnings as errors" OFF)
option(SEQLZ_SANITIZE "Build with ASan and UBSan" OFF)
# MSan, clang only and not together with ASan. Everything that is linked must be built with it, so it is
# for the fuzz targets of fuzz/, which are C: the C++ standard library is not built with it.
option(SEQLZ_MSAN "Build with MSan" OFF)

add_library(seqlz_warnings INTERFACE)
target_compile_options(seqlz_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(SEQLZ_WERROR)
    target_compile_options(seqlz_warnings INTERFACE -Werror)
endif()
if(SEQLZ_SANITIZE)
    target_compile_options(seqlz_warnings INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(seqlz_warnings INTERFACE -fsanitize=address,undefined)
endif()
if(SEQLZ_MSAN)
    target_compile_options(seqlz_warnings INTERFACE -fsanitize=memory -fsanitize-memory-track-origins
                                                       -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(seqlz_warnings INTERFACE -fsanitize=memory)
endif()

# Page corpus collectors, see docs/plan.md Phase 1. Linux only, they read /proc.
add_library(seqlz_collect STATIC tools/collect/page_stats.cpp tools/collect/resident.cpp tools/collect/split.cpp
                                   tools/collect/import_raw.cpp)
target_include_directories(seqlz_collect PUBLIC tools/collect)
target_link_libraries(seqlz_collect PRIVATE seqlz_warnings)

add_executable(seqlz-collect-resident tools/collect/collect_resident_main.cpp)
target_link_libraries(seqlz-collect-resident PRIVATE seqlz_collect seqlz_warnings)

add_executable(seqlz-split-corpus tools/collect/split_main.cpp)
target_link_libraries(seqlz-split-corpus PRIVATE seqlz_collect seqlz_warnings)

add_executable(seqlz-sample-corpus tools/collect/sample_main.cpp)
target_link_libraries(seqlz-sample-corpus PRIVATE seqlz_collect seqlz_warnings)

add_executable(seqlz-import-raw tools/collect/import_raw_main.cpp)
target_link_libraries(seqlz-import-raw PRIVATE seqlz_collect seqlz_warnings)

add_library(seqlz_bench STATIC bench/zsmalloc_cost.cpp bench/harness.cpp bench/compare.cpp bench/score.cpp)
target_include_directories(seqlz_bench PUBLIC bench)
find_package(Threads REQUIRED)
target_link_libraries(seqlz_bench PUBLIC seqlz_collect Threads::Threads PRIVATE seqlz_warnings)

add_executable(seqlz-compare bench/compare_main.cpp)
target_link_libraries(seqlz-compare PRIVATE seqlz_bench seqlz_warnings)

add_executable(seqlz-score bench/score_main.cpp)
target_link_libraries(seqlz-score PRIVATE seqlz_bench seqlz_warnings)

add_executable(seqlz-swap-bursts tools/swap-bursts/swap-bursts.c)
target_link_libraries(seqlz-swap-bursts PRIVATE seqlz_warnings)

# seqlz's codes and decode tables, generated from the code lengths by a program for the build host
set(SEQLZ_TABLES ${CMAKE_BINARY_DIR}/seqlz_tables.c)
file(GLOB SEQLZ_HEADERS CONFIGURE_DEPENDS src/*.h src/*.inc)
add_custom_command(OUTPUT ${SEQLZ_TABLES}
    COMMAND ${CMAKE_SOURCE_DIR}/tools/gen-tables/gen-tables.sh ${SEQLZ_TABLES}
        -DSEQLZ_PAGE_BITS=${SEQLZ_PAGE_BITS}
    DEPENDS tools/gen-tables/gen-tables.sh tools/gen-tables/gen_seqlz_tables.c src/seqlz_codes.c
        src/seqlz_default_tables.c src/seqlz_lit_sets.c ${SEQLZ_HEADERS}
    COMMENT "seqlz's tables"
    VERBATIM)
# it includes seqlz_tables.h, and is not in src/ next to it
set_source_files_properties(${SEQLZ_TABLES} PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/src)

# seqlz without kernel code, for the tests and tools. seqlz's sources, for every target that builds it:
# this library, the kernel codecs, the fuzzers
set(SEQLZ_SOURCES src/seqlz_codes.c src/seqlz_compress.c src/seqlz_compress_literals.c src/seqlz_decompress.c
    src/seqlz_decompress_literals.c src/seqlz_default_tables.c src/seqlz_lit_sets.c ${SEQLZ_TABLES})
add_library(seqlz STATIC ${SEQLZ_SOURCES})
target_include_directories(seqlz PUBLIC src)
target_link_libraries(seqlz PRIVATE seqlz_warnings)

# the tables' training, and building them from the counts of bench/seqlz_counts_*.txt
add_library(seqlz_train STATIC bench/seqlz_train.cpp)
target_include_directories(seqlz_train PUBLIC bench)
target_link_libraries(seqlz_train PUBLIC seqlz PRIVATE seqlz_warnings)
add_executable(seqlz-train bench/seqlz_train_main.cpp)
target_link_libraries(seqlz-train PRIVATE seqlz_train seqlz_bench seqlz_warnings)

# pages made to be slow for seqlz, for its worst-case time
add_executable(seqlz-worst tools/seqlz-worst/worst.c)
target_link_libraries(seqlz-worst PRIVATE seqlz seqlz_warnings m)

# The fuzz targets of fuzz/, in libFuzzer's interface: built with clang they are libFuzzer binaries,
# built with AFL++'s afl-clang-lto as CC they run under afl-fuzz. seqlz itself gets the coverage
# instrumentation too, in a library of its own; make_seeds uses the plain one.
option(SEQLZ_FUZZ "Build the fuzz targets of fuzz/, needs clang or afl-clang-lto" OFF)
if(SEQLZ_FUZZ)
    add_library(seqlz_fuzz STATIC ${SEQLZ_SOURCES})
    target_include_directories(seqlz_fuzz PUBLIC src)
    # afl-clang-lto instruments every file anyway, and with this flag it would need libFuzzer's runtime
    if(NOT CMAKE_C_COMPILER MATCHES "afl-")
        target_compile_options(seqlz_fuzz PRIVATE -fsanitize=fuzzer-no-link)
    endif()
    target_link_libraries(seqlz_fuzz PRIVATE seqlz_warnings)
    foreach(target seqlz_decode_fuzz seqlz_roundtrip_fuzz seqlz_diff_fuzz seqlz_encode_fuzz)
        add_executable(${target} fuzz/${target}.c)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_libraries(${target} PRIVATE seqlz_fuzz seqlz_warnings)
    endforeach()
    # the decoder written from docs/format.md, instrumented like the target
    target_sources(seqlz_diff_fuzz PRIVATE tools/seqlz-ref/seqlz_ref.c)
    target_include_directories(seqlz_diff_fuzz PRIVATE tools/seqlz-ref)
    # Searches for slow pages, see tools/seqlz-worst/cost_fuzz.c; not part of afl.sh or smoke.sh. With
    # seqlz without coverage instrumentation, which made some pages 3.6 and others 6.6 times costlier
    # and so changed which ones cost most: the only coverage is the target's own.
    foreach(target seqlz_compress_cost_fuzz seqlz_decode_cost_fuzz seqlz_sequence_cost_fuzz)
        add_executable(${target} tools/seqlz-worst/cost_fuzz.c)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_libraries(${target} PRIVATE seqlz seqlz_warnings)
    endforeach()
    target_compile_definitions(seqlz_compress_cost_fuzz PRIVATE COST_COMPRESS)
    target_compile_definitions(seqlz_sequence_cost_fuzz PRIVATE COST_SEQUENCES)
    add_executable(seqlz-fuzz-seeds fuzz/make_seeds.c)
    target_link_libraries(seqlz-fuzz-seeds PRIVATE seqlz seqlz_warnings)
endif()

include(cmake/kernel_codecs.cmake)

# The search for slow pages on the kernel's codecs as zram calls them, all with kernel flags, see
# tools/seqlz-worst/cost_fuzz.c
if(SEQLZ_FUZZ AND SEQLZ_HAVE_KERNEL_CODECS)
    foreach(codec lz4 lzo_rle zstd seqlz_2)
        set(lib seqlz_kernel_${codec})
        if(codec STREQUAL "lzo_rle")
            set(lib seqlz_kernel_lzo)
        elseif(codec STREQUAL "seqlz_2")
            set(lib seqlz_kernel_seqlz)
        endif()
        add_executable(${codec}_cost_fuzz tools/seqlz-worst/cost_fuzz.c)
        target_compile_definitions(${codec}_cost_fuzz PRIVATE COST_CODEC=bench_codec_${codec})
        target_compile_options(${codec}_cost_fuzz PRIVATE -fsanitize=fuzzer)
        target_link_options(${codec}_cost_fuzz PRIVATE -fsanitize=fuzzer)
        target_include_directories(${codec}_cost_fuzz PRIVATE src bench/kernel_codecs)
        target_link_libraries(${codec}_cost_fuzz PRIVATE ${lib} seqlz_warnings)
    endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.5.3
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(doctest)

    add_executable(seqlz_test test/main.cpp test/zsmalloc_cost_test.cpp test/page_stats_test.cpp test/resident_test.cpp
                                 test/harness_test.cpp test/split_test.cpp test/import_raw_test.cpp
                                 test/compare_test.cpp test/score_test.cpp test/seqlz_test.cpp test/seqlz_ref_test.cpp tools/seqlz-ref/seqlz_ref.c
                                 test/seqlz_train_test.cpp)
    target_link_libraries(seqlz_test PRIVATE seqlz_bench seqlz_collect seqlz seqlz_train
                                                seqlz_warnings doctest::doctest)
    # where the test finds bench/seqlz_counts_*.txt
    target_compile_definitions(seqlz_test PRIVATE SEQLZ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_include_directories(seqlz_test PRIVATE tools/seqlz-ref)
    if(SEQLZ_HAVE_KERNEL_CODECS)
        target_sources(seqlz_test PRIVATE test/kernel_codecs_test.cpp test/lz_analysis_test.cpp bench/lz_analysis.cpp)
        target_link_libraries(seqlz_test PRIVATE seqlz_kernel_lz4 seqlz_kernel_lzo seqlz_kernel_zstd
                                                    seqlz_kernel_seqlz)
        target_compile_definitions(seqlz_test PRIVATE SEQLZ_HAVE_KERNEL_CODECS)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            add_test(NAME kernel_seqlz_prefetch
                     COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP}
                             "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:seqlz_kernel_seqlz>,|>"
                             -P ${CMAKE_SOURCE_DIR}/cmake/check_prefetch.cmake)
        endif()
    endif()
    add_test(NAME seqlz_test COMMAND seqlz_test)
endif()
