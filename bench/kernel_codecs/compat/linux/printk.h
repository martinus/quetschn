/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_LINUX_PRINTK_H
#define SEQLZ_LINUX_PRINTK_H

/* Only used by zstd at DEBUGLEVEL >= 2, which the kernel does not build. */
#define pr_debug(...) ((void)0)

#endif
