/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/*
 * Forced include for kernel sources built in userspace, in place of the kernel's generated config.
 * Only the symbols that lib/lz4 and lib/lzo look at, with the values a 64-bit x86 or arm64 kernel has.
 * The kernel only ever defines one of __LITTLE_ENDIAN and __BIG_ENDIAN; glibc defines both, which is why
 * these sources are compiled with -nostdinc and never see a libc header.
 */
#ifndef QUETSCHN_KCONFIG_H
#define QUETSCHN_KCONFIG_H

#if defined(__x86_64__)
#    define CONFIG_X86 1
#    define CONFIG_X86_64 1
#elif defined(__aarch64__)
#    define CONFIG_ARM64 1
#else
#    error "only x86-64 and arm64 are supported"
#endif

#define CONFIG_64BIT 1
#define CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS 1
#define BITS_PER_LONG 64

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#    define __LITTLE_ENDIAN 1234
#else
#    define __BIG_ENDIAN 4321
#endif

#endif
