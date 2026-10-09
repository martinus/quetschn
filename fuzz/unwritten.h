// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * unwritten(): under MSan, marks a buffer as not written, so that a read of a byte the codec did not
 * write for this input is reported where its value decides something, also when the buffer still holds
 * what an earlier input left there. The bytes stay as they are, so a run behaves the same again. The
 * targets fill their buffers with known bytes before, which is all there is without MSan.
 */
#ifndef QUETSCHN_FUZZ_UNWRITTEN_H
#define QUETSCHN_FUZZ_UNWRITTEN_H

#include <stddef.h>

#if defined(__has_feature)
#    if __has_feature(memory_sanitizer)
#        include <sanitizer/msan_interface.h>
#        define UNWRITTEN_MSAN 1
#    endif
#endif

static inline void unwritten(const volatile void* p, size_t n) {
#ifdef UNWRITTEN_MSAN
    __msan_poison(p, n);
#else
    (void)p;
    (void)n;
#endif
}

#endif
