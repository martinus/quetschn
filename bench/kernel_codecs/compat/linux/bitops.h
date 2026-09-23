/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_LINUX_BITOPS_H
#define QUETSCHN_LINUX_BITOPS_H

#include <linux/types.h>

/* index of the lowest and highest set bit, undefined for 0, like the kernel's */
static __always_inline unsigned long __ffs(unsigned long word) {
    return (unsigned long)__builtin_ctzl(word);
}

static __always_inline unsigned long __fls(unsigned long word) {
    return (unsigned long)(BITS_PER_LONG - 1 - __builtin_clzl(word));
}

#endif
