/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_LINUX_UNALIGNED_H
#define QUETSCHN_LINUX_UNALIGNED_H

#include <linux/types.h>

/* Same technique as the kernel: a packed struct, so the compiler emits a plain load or store on
 * architectures with efficient unaligned access. */
#define __get_unaligned_t(type, ptr)                                   \
    ({                                                                 \
        const struct {                                                 \
            type x;                                                    \
        } __attribute__((__packed__))* __pptr = (typeof(__pptr))(ptr); \
        __pptr->x;                                                     \
    })

#define __put_unaligned_t(type, val, ptr)                              \
    do {                                                               \
        struct {                                                       \
            type x;                                                    \
        } __attribute__((__packed__))* __pptr = (typeof(__pptr))(ptr); \
        __pptr->x = (val);                                             \
    } while (0)

#define get_unaligned(ptr) __get_unaligned_t(typeof(*(ptr)), (ptr))
#define put_unaligned(val, ptr) __put_unaligned_t(typeof(*(ptr)), (val), (ptr))

#if defined(__LITTLE_ENDIAN)
#    define __q_le16(x) (x)
#    define __q_le32(x) (x)
#    define __q_le64(x) (x)
#    define __q_be32(x) __builtin_bswap32(x)
#    define __q_be64(x) __builtin_bswap64(x)
#else
#    define __q_le16(x) __builtin_bswap16(x)
#    define __q_le32(x) __builtin_bswap32(x)
#    define __q_le64(x) __builtin_bswap64(x)
#    define __q_be32(x) (x)
#    define __q_be64(x) (x)
#endif

static __always_inline u16 get_unaligned_le16(const void* p) {
    return __q_le16(__get_unaligned_t(u16, p));
}

static __always_inline u32 get_unaligned_le32(const void* p) {
    return __q_le32(__get_unaligned_t(u32, p));
}

static __always_inline u64 get_unaligned_le64(const void* p) {
    return __q_le64(__get_unaligned_t(u64, p));
}

static __always_inline void put_unaligned_le16(u16 val, void* p) {
    __put_unaligned_t(u16, __q_le16(val), p);
}

static __always_inline void put_unaligned_le32(u32 val, void* p) {
    __put_unaligned_t(u32, __q_le32(val), p);
}

static __always_inline void put_unaligned_le64(u64 val, void* p) {
    __put_unaligned_t(u64, __q_le64(val), p);
}

static __always_inline u32 get_unaligned_be32(const void* p) {
    return __q_be32(__get_unaligned_t(u32, p));
}

static __always_inline u64 get_unaligned_be64(const void* p) {
    return __q_be64(__get_unaligned_t(u64, p));
}

static __always_inline void put_unaligned_be32(u32 val, void* p) {
    __put_unaligned_t(u32, __q_be32(val), p);
}

static __always_inline void put_unaligned_be64(u64 val, void* p) {
    __put_unaligned_t(u64, __q_be64(val), p);
}

#endif
