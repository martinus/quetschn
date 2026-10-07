# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# The kernel's own compressors, built from a Linux source tree for the benchmark harness (docs/plan.md §5.1).
# The kernel sources are compiled from QUETSCHN_KERNEL_TREE and never copied into this repository:
# lib/lzo is GPL-2.0-only. The quetschn-bench-* binaries link them and are therefore GPL-2.0 works;
# they are measurement tools and not distributed.
#
# Flags: from `make V=1 lib/lz4/lz4_compress.o lib/zstd/compress/zstd_compress.o` of kernel 986c24e0fe44.
# On x86-64 with Fedora's config for 7.1.8 and gcc 16, on arm64 with defconfig and the Android NDK r30's
# clang 21, the compiler Android kernels are built with. Everything that changes code generation and
# works in userspace is kept. Left out, each for a reason:
# - -mcmodel=kernel, -mstack-protector-guard-*, -fno-PIE: the kernel's address space, not ours.
# - -mindirect-branch=thunk-extern, -mfunction-return=thunk-extern: the kernel patches these call sites
#   at boot depending on the CPU's vulnerabilities, so the compile-time flag does not say what runs.
# - -pg, -mfentry, -mrecord-mcount, -fpatchable-function-entry: ftrace NOPs at function entry.
# - -mpreferred-stack-boundary=3: would call libc's memcpy with a stack the x86-64 ABI does not allow.
# - -fshort-wchar, -fms-extensions, -fno-var-tracking, -fintegrated-as: no effect on these codecs' code.
# Flags the compiler does not know (older gcc, clang) are dropped with a message.

set(QUETSCHN_KERNEL_TREE "" CACHE PATH "Linux source tree to build the kernel codecs from")

if(NOT QUETSCHN_KERNEL_TREE)
    message(STATUS "QUETSCHN_KERNEL_TREE not set: kernel codecs and quetschn-bench-* are not built")
    return()
endif()
if(NOT EXISTS "${QUETSCHN_KERNEL_TREE}/lib/lz4/lz4_compress.c")
    message(FATAL_ERROR "QUETSCHN_KERNEL_TREE=${QUETSCHN_KERNEL_TREE} is not a Linux source tree")
endif()

enable_language(C)
include(CheckCCompilerFlag)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    # arm64 defconfig has no UBSAN, unlike Fedora's x86-64 config.
    set(quetschn_kernel_flags_wanted
        -std=gnu11 -O2
        -fno-strict-aliasing -fno-strict-overflow -fno-delete-null-pointer-checks -fno-common -funsigned-char
        -fno-omit-frame-pointer -fno-optimize-sibling-calls -falign-functions=4
        -ftrivial-auto-var-init=zero -fstrict-flex-arrays=3
        -fstack-protector-strong -fno-stack-clash-protection -fno-asynchronous-unwind-tables -fno-unwind-tables
        -mlittle-endian -mgeneral-regs-only -mbranch-protection=pac-ret)
else()
    set(quetschn_kernel_flags_wanted
        -std=gnu11 -O2
        -fno-strict-aliasing -fno-strict-overflow -fno-delete-null-pointer-checks -fno-common -funsigned-char
        -fno-allow-store-data-races -fno-jump-tables -falign-jumps=1 -falign-loops=1
        -fno-inline-functions-called-once -fmin-function-alignment=16 -fconserve-stack
        -ftrivial-auto-var-init=zero -fzero-init-padding-bits=all -fstrict-flex-arrays=3
        -fstack-protector-strong -fno-stack-clash-protection -fno-asynchronous-unwind-tables
        -fsanitize=bounds-strict -fsanitize=shift)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        list(APPEND quetschn_kernel_flags_wanted
            -march=x86-64 -mtune=generic -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx -mno-sse4a
            -mno-80387 -mno-fp-ret-in-387 -mno-red-zone -mskip-rax-setup -fcf-protection=branch -mharden-sls=all)
    endif()
endif()

set(QUETSCHN_KERNEL_CFLAGS "")
foreach(flag IN LISTS quetschn_kernel_flags_wanted)
    string(MAKE_C_IDENTIFIER "HAVE_C${flag}" var)
    check_c_compiler_flag("${flag}" ${var})
    if(${var})
        list(APPEND QUETSCHN_KERNEL_CFLAGS ${flag})
    else()
        message(STATUS "kernel codecs: ${CMAKE_C_COMPILER_ID} does not know ${flag}, dropped")
    endif()
endforeach()

# For A/B runs of two builds: every function on its own 64 bytes, so that a change in one function does
# not move the ones behind it. With the kernel's 4 bytes on arm64, a change in count() moved
# seqlz_decode() by 12 bytes, and that alone made its reads on the Mi 9T's little core 130 ns slower,
# while two runs of the same build differ by 10 to 20 ns (docs/explored-designs.md).
option(QUETSCHN_ALIGN_FUNCTIONS "Align every kernel codec function to 64 bytes, for A/B comparisons" OFF)
if(QUETSCHN_ALIGN_FUNCTIONS)
    list(APPEND QUETSCHN_KERNEL_CFLAGS -falign-functions=64)
endif()

# Like the kernel: no libc headers. The compiler's own (stddef.h and friends) stay reachable.
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
                OUTPUT_VARIABLE quetschn_cc_include OUTPUT_STRIP_TRAILING_WHITESPACE)

# The public headers are copied into the build tree, so that the include path does not also expose the
# rest of the kernel's include/ (which would pull in the real kernel headers instead of compat/).
set(quetschn_kernel_include "${CMAKE_BINARY_DIR}/kernel_include")
foreach(h lz4.h lzo.h zstd.h zstd_lib.h zstd_errors.h xxhash.h)
    configure_file("${QUETSCHN_KERNEL_TREE}/include/linux/${h}" "${quetschn_kernel_include}/linux/${h}" COPYONLY)
endforeach()

function(quetschn_kernel_codec target libdir extra_flags)
    add_library(${target} STATIC ${ARGN})
    target_compile_options(${target} PRIVATE ${QUETSCHN_KERNEL_CFLAGS} ${extra_flags} -nostdinc
                           -include "${CMAKE_SOURCE_DIR}/bench/kernel_codecs/compat/kconfig.h")
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/bench/kernel_codecs/compat" "${quetschn_kernel_include}"
        "${QUETSCHN_KERNEL_TREE}/${libdir}" "${CMAKE_SOURCE_DIR}/bench/kernel_codecs")
    # as an option: CMake drops the compiler's own directory from target_include_directories() as implicit
    target_compile_options(${target} PRIVATE "SHELL:-isystem ${quetschn_cc_include}")
    target_link_libraries(${target} PUBLIC quetschn_kernel_runtime)
endfunction()

# Built normally, with libc: the UBSAN handlers, and the allocator that stands in for kzalloc/vzalloc
add_library(quetschn_kernel_runtime STATIC bench/kernel_codecs/ubsan_stubs.c bench/kernel_codecs/alloc.c)
target_include_directories(quetschn_kernel_runtime PUBLIC bench/kernel_codecs)

# lib/lz4/Makefile adds -O3. Target options come after CMake's CMAKE_C_FLAGS_<CONFIG>, so the last -O on
# the command line is always ours: -O3 for lz4, -O2 for lzo, whatever the build type.
quetschn_kernel_codec(quetschn_kernel_lz4 lib/lz4 -O3
    "${QUETSCHN_KERNEL_TREE}/lib/lz4/lz4_compress.c"
    "${QUETSCHN_KERNEL_TREE}/lib/lz4/lz4_decompress.c"
    "${QUETSCHN_KERNEL_TREE}/lib/lz4/lz4hc_compress.c"
    bench/kernel_codecs/zram_lz4.c
    bench/kernel_codecs/zram_lz4hc.c)
quetschn_kernel_codec(quetschn_kernel_lzo lib/lzo ""
    "${QUETSCHN_KERNEL_TREE}/lib/lzo/lzo1x_compress.c"
    "${QUETSCHN_KERNEL_TREE}/lib/lzo/lzo1x_decompress_safe.c"
    bench/kernel_codecs/zram_lzo.c)

# lib/zstd/Makefile builds these three modules; zstd needs lib/xxhash.c as well. No extra flags: the
# V=1 line for lib/zstd has exactly the same flags as lib/lz4 minus -O3.
file(GLOB_RECURSE quetschn_zstd_sources "${QUETSCHN_KERNEL_TREE}/lib/zstd/*.c")
quetschn_kernel_codec(quetschn_kernel_zstd lib/zstd ""
    ${quetschn_zstd_sources}
    "${QUETSCHN_KERNEL_TREE}/lib/xxhash.c"
    bench/kernel_codecs/zram_zstd.c)

# seqlz with lz4's flags, -O3 included
quetschn_kernel_codec(quetschn_kernel_seqlz lib/lz4 -O3 ${QUETSCHN_SEQLZ_SOURCES} src/zram_seqlz.c)
target_include_directories(quetschn_kernel_seqlz PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_executable(quetschn-seqlz-train bench/seqlz_train_main.cpp)
target_include_directories(quetschn-seqlz-train PRIVATE src)
target_link_libraries(quetschn-seqlz-train PRIVATE quetschn_bench quetschn_kernel_seqlz quetschn_warnings)

foreach(codec lz4 lz4hc lzo lzo_rle zstd seqlz_fast seqlz_fast_lit)
    string(REPLACE "_" "-" name ${codec})
    if(codec MATCHES "^lz4")
        set(lib quetschn_kernel_lz4)
    elseif(codec STREQUAL "zstd")
        set(lib quetschn_kernel_zstd)
    elseif(codec MATCHES "^seqlz")
        set(lib quetschn_kernel_seqlz)
    else()
        set(lib quetschn_kernel_lzo)
    endif()
    add_executable(quetschn-bench-${name} bench/bench_main.cpp)
    target_compile_definitions(quetschn-bench-${name} PRIVATE QUETSCHN_CODEC=quetschn_codec_${codec})
    target_link_libraries(quetschn-bench-${name} PRIVATE quetschn_bench ${lib} quetschn_warnings)
endforeach()

# All codecs in one binary, timing interleaved per page, see bench_main.cpp
add_executable(quetschn-bench-interleaved bench/bench_main.cpp)
target_compile_definitions(quetschn-bench-interleaved PRIVATE QUETSCHN_INTERLEAVED)
target_link_libraries(quetschn-bench-interleaved PRIVATE quetschn_bench quetschn_kernel_lz4 quetschn_kernel_lzo
                                                         quetschn_kernel_zstd quetschn_kernel_seqlz quetschn_warnings)

# Where the ratio of zstd comes from: lz4hc's matches, costed with entropy coding
add_executable(quetschn-lz-analysis bench/lz_analysis_main.cpp bench/lz_analysis.cpp)
target_include_directories(quetschn-lz-analysis PRIVATE src)
target_link_libraries(quetschn-lz-analysis PRIVATE quetschn_bench quetschn_kernel_lz4 quetschn_kernel_seqlz quetschn_warnings)

set(QUETSCHN_HAVE_KERNEL_CODECS ON)
