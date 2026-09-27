# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# Fails if seqlz.c, built with the kernel's flags, has no prefetch instruction. The kernel builds x86-64
# without SSE, and clang then drops __builtin_prefetch without a warning, see PAGE_LZ_PREFETCH in
# explore/page_lz.h. Run by ctest: cmake -DOBJDUMP=... -DOBJECTS=a.o|b.o -P check_prefetch.cmake
string(REPLACE "|" ";" objects "${OBJECTS}")
list(FILTER objects INCLUDE REGEX "/seqlz\\.c\\.o(bj)?$")
if(NOT objects)
    message(FATAL_ERROR "no seqlz.c object in ${OBJECTS}")
endif()
execute_process(COMMAND "${OBJDUMP}" -d ${objects} OUTPUT_VARIABLE dis RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "${OBJDUMP} failed on ${objects}")
endif()
string(REGEX MATCHALL "prefetcht0" hits "${dis}")
list(LENGTH hits n)
if(n EQUAL 0)
    message(FATAL_ERROR "${objects}: no prefetcht0, the decoder's prefetches were dropped")
endif()
message(STATUS "${objects}: ${n} prefetcht0")
