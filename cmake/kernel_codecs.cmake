# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# The kernel's own compressors, built from a Linux source tree for the benchmark harness (PLAN.md §5.1).
# The kernel sources are compiled from QUETSCHN_KERNEL_TREE and never copied into this repository:
# lib/lzo is GPL-2.0-only. The quetschn-bench-* binaries link them and are therefore GPL-2.0 works;
# they are measurement tools and not distributed.
#
# Flags: from `make V=1 lib/lz4/lz4_compress.o lib/zstd/compress/zstd_compress.o` with Fedora's config for 7.1.8 on x86-64 (kernel
# 986c24e0fe44, gcc 16). Everything that changes code generation and works in userspace is kept.
# Left out, each for a reason:
# - -mcmodel=kernel, -mstack-protector-guard-*, -fno-PIE: the kernel's address space, not ours.
# - -mindirect-branch=thunk-extern, -mfunction-return=thunk-extern: the kernel patches these call sites
#   at boot depending on the CPU's vulnerabilities, so the compile-time flag does not say what runs.
# - -pg, -mfentry, -mrecord-mcount, -fpatchable-function-entry: ftrace NOPs at function entry.
# - -mpreferred-stack-boundary=3: would call libc's memcpy with a stack the x86-64 ABI does not allow.
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
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    # Not yet taken from a real arm64 build, see the TODO in PLAN.md next actions.
    list(APPEND quetschn_kernel_flags_wanted -mgeneral-regs-only)
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
    target_include_directories(${target} SYSTEM PRIVATE "${quetschn_cc_include}")
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
    bench/kernel_codecs/zram_lz4.c)
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

# The spike with the same flags as lz4, -O3 included, so the comparison is about the code
quetschn_kernel_codec(quetschn_kernel_spike lib/lz4 -O3 spike/wk64.c spike/zram_spike.c)
target_include_directories(quetschn_kernel_spike PRIVATE "${CMAKE_SOURCE_DIR}/spike")

foreach(codec lz4 lzo lzo_rle zstd spike_switch spike_branchless spike_zeroskip spike_slots)
    string(REPLACE "_" "-" name ${codec})
    if(codec STREQUAL "lz4")
        set(lib quetschn_kernel_lz4)
    elseif(codec STREQUAL "zstd")
        set(lib quetschn_kernel_zstd)
    elseif(codec MATCHES "^spike")
        set(lib quetschn_kernel_spike)
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
                                                         quetschn_kernel_zstd quetschn_kernel_spike quetschn_warnings)

set(QUETSCHN_HAVE_KERNEL_CODECS ON)
