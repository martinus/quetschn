/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_LINUX_KERNEL_H
#define QUETSCHN_LINUX_KERNEL_H

#include <linux/string.h>
#include <linux/types.h>

/* Without the kernel's type checks, but each argument is evaluated once, like the kernel's. */
#define min(a, b)              \
    ({                         \
        __auto_type __a = (a); \
        __auto_type __b = (b); \
        __a < __b ? __a : __b; \
    })
#define max(a, b)              \
    ({                         \
        __auto_type __a = (a); \
        __auto_type __b = (b); \
        __a > __b ? __a : __b; \
    })
#define min_t(type, a, b) min((type)(a), (type)(b))
#define max_t(type, a, b) max((type)(a), (type)(b))

#endif
