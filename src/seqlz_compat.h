/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_COMPAT_H
#define SEQLZ_COMPAT_H

/*
 * The kernel's names for what the codec needs: types, attributes, unaligned
 * access, min(). In a kernel build they come from the kernel's headers.
 * Everywhere else (the tests, the benchmarks, the fuzzers and the tools) the
 * same names are defined here, from the compiler's freestanding headers, as
 * lib/bootconfig.c and lib/decompress_unxz.c share their code with tools/.
 * Nothing here defines bool, true, false or NULL itself, so that seqlz.h stays
 * usable from C++.
 */

#ifdef __KERNEL__
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/errno.h>
/* min(), in linux/kernel.h before 5.10 */
#if __has_include(<linux/minmax.h>)
#include <linux/minmax.h>
#else
#include <linux/kernel.h>
#endif
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
/* before 6.12 it was asm/unaligned.h, as in the Mi 9T's 4.14 */
#if __has_include(<linux/unaligned.h>)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif
/* static_assert() came with 5.1 */
#ifndef static_assert
#define static_assert _Static_assert
#endif
/* sizeof_field() came with 5.5 */
#ifndef sizeof_field
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
#endif
#else

#include <stdbool.h>
#include <stddef.h>

/*
 * With libc, its declarations. The benchmarks also build the codec with the
 * kernel's flags and without libc headers, as the kernel is built
 * (cmake/kernel_codecs.cmake); then only these prototypes.
 */
#if __has_include(<string.h>)
#include <string.h>
#else
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
#endif
#if __has_include(<errno.h>)
#include <errno.h>
#else
#define EINVAL 22
#endif

/* as the kernel has them on every architecture, u64 too */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* glibc's sys/cdefs.h has its own */
#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif
/* only for the codec's C: as macros they would break C++'s [[gnu::noinline]] */
#ifndef __cplusplus
#define noinline __attribute__((__noinline__))
#define __cold __attribute__((__cold__))
#define __aligned(x) __attribute__((__aligned__(x)))
#endif

/* a keyword in C23 and C++ */
#if !defined(__cplusplus) && !defined(static_assert) && \
	(!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define static_assert _Static_assert
#endif

#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))

/* the kernel's min() is a macro, which would break std::min in C++ */
static inline unsigned int min(unsigned int a, unsigned int b)
{
	return a < b ? a : b;
}

/*
 * Little and big endian numbers at any address, whatever the CPU. On x86-64 and
 * arm64 a plain load or store, and a byte swap for big endian.
 */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SEQLZ_LE32(x) __builtin_bswap32(x)
#define SEQLZ_LE64(x) __builtin_bswap64(x)
#define SEQLZ_BE64(x) (x)
#else
#define SEQLZ_LE32(x) (x)
#define SEQLZ_LE64(x) (x)
#define SEQLZ_BE64(x) __builtin_bswap64(x)
#endif

static __always_inline u32 get_unaligned_le32(const void *p)
{
	u32 v;

	memcpy(&v, p, sizeof(v));
	return SEQLZ_LE32(v);
}

static __always_inline u64 get_unaligned_le64(const void *p)
{
	u64 v;

	memcpy(&v, p, sizeof(v));
	return SEQLZ_LE64(v);
}

static __always_inline u64 get_unaligned_be64(const void *p)
{
	u64 v;

	memcpy(&v, p, sizeof(v));
	return SEQLZ_BE64(v);
}

static __always_inline void put_unaligned_le64(u64 val, void *p)
{
	val = SEQLZ_LE64(val);
	memcpy(p, &val, sizeof(val));
}

static __always_inline void put_unaligned_be64(u64 val, void *p)
{
	val = SEQLZ_BE64(val);
	memcpy(p, &val, sizeof(val));
}

#endif

#endif
