// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Kernels built with CONFIG_UBSAN_BOUNDS and CONFIG_UBSAN_SHIFT (Fedora's, among others) compile lib/
 * with -fsanitize=bounds-strict,shift and call report handlers the kernel implements itself. The
 * userspace build uses the same flags so that the code is the same, and these handlers stand in for the
 * kernel's. Weak, so that libubsan's win in a -fsanitize=undefined build of the harness.
 */
#include <stdio.h>
#include <stdlib.h>

#define QUETSCHN_UBSAN_HANDLER(name)                                \
    __attribute__((weak)) void name(void* data, void* a, void* b);  \
    __attribute__((weak)) void name(void* data, void* a, void* b) { \
        (void)data;                                                 \
        (void)a;                                                    \
        (void)b;                                                    \
        fprintf(stderr, "kernel codec: UBSAN report %s\n", #name);  \
        abort();                                                    \
    }

QUETSCHN_UBSAN_HANDLER(__ubsan_handle_out_of_bounds)
QUETSCHN_UBSAN_HANDLER(__ubsan_handle_out_of_bounds_abort)
QUETSCHN_UBSAN_HANDLER(__ubsan_handle_shift_out_of_bounds)
QUETSCHN_UBSAN_HANDLER(__ubsan_handle_shift_out_of_bounds_abort)
