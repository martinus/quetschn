/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_LINUX_MATH64_H
#define QUETSCHN_LINUX_MATH64_H

#include <linux/types.h>

/* On 64-bit kernels div_u64 is a plain division too. */
static __always_inline u64 div_u64(u64 dividend, u32 divisor) {
    return dividend / divisor;
}

#endif
