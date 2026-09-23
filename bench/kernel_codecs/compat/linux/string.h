/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_LINUX_STRING_H
#define QUETSCHN_LINUX_STRING_H

#include <linux/types.h>

/* Declared here instead of taken from libc, see kconfig.h. They link to the libc functions, which is
 * also what the kernel's own memcpy/memset are for these sizes: out of line, and the compiler inlines
 * the small fixed-size cases either way. */
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);

#endif
