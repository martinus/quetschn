/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_LINUX_COMPILER_H
#define QUETSCHN_LINUX_COMPILER_H

#define __always_inline inline __attribute__((__always_inline__))
#define noinline __attribute__((__noinline__))
#define __maybe_unused __attribute__((__unused__))
#define fallthrough __attribute__((__fallthrough__))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define BUILD_BUG_ON(cond) _Static_assert(!(cond), "BUILD_BUG_ON: " #cond)

#endif
