/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_LINUX_STDDEF_H
#define SEQLZ_LINUX_STDDEF_H

#include <linux/types.h>

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
