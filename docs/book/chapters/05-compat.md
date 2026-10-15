# seqlz_compat.h: the kernel's names, outside the kernel {#ch-compat}

Every file of `src/` is written as if it were in the kernel already: `u8` and `u64`,
`get_unaligned_le64()`, `prefetch()`, `sizeof_field()`, `min()`, the names a reviewer of `lib/`
expects. The same files also build as ordinary programs, the tests, the benchmarks, the fuzzers and
the tools. `src/seqlz_compat.h` makes both work: in a kernel build it includes the kernel's headers,
everywhere else it defines the same names from the few headers a C compiler brings along. When
`tools/kernel-port/port.py` writes the kernel's copy, it replaces `#include "seqlz_compat.h"` with the
kernel's headers, and nothing else in the code changes. There are three ways `src/` gets compiled:

| build | who does it | where the names come from |
| --- | --- | --- |
| the kernel's copy | `tools/kernel-port/port.py` writes `lib/seqlz/` into a kernel tree | the kernel's headers; `seqlz_compat.h` is not in the copy |
| `src/` as it is, inside a kernel | `tools/zram-vm/run.sh` (the kernel VM) and `tools/zram-phone/build.sh` (a kernel module for the Mi 9T's 4.14, [chapter 15](#ch-zram-glue--init)) | the first half of this header, `__KERNEL__` is defined |
| userspace | tests, benchmarks, fuzzers, tools, built with CMake | the second half of this header |

The middle row is why the first half has fallbacks for old kernels: the phone runs Linux 4.14, from
2017.

## The licence and the promise {#ch-compat--top}

@listing src/seqlz_compat.h 1-14

`MIT OR GPL-2.0-only` lets the user pick one of the two: the kernel needs GPL-2.0, and MIT lets
userspace programs and other systems use the code too, as `zstd` is licensed
([`docs/plan.md`](https://github.com/martinus/quetschn/blob/main/docs/plan.md#project-constraints)).
`port.py` writes the kernel's copy with the two the other way round, `GPL-2.0-only OR MIT`, the
kernel's order. The pattern of the header has precedent in `lib/`: the kernel's own `tools/` builds
`lib/bootconfig.c`, whose `#ifdef __KERNEL__` picks the kernel's headers only in a kernel build, and
`lib/rbtree.c` against the stand-in headers of `tools/include/`. The comment named
`lib/decompress_unxz.c` until 11th October; nothing in `tools/` builds that one
([#183](https://github.com/martinus/quetschn/issues/183)).

Lines 12 and 13 shape the rest of the file: the test files and the benchmarks are C++ and include
`seqlz.h` inside `extern "C" { }`, so a macro named `bool`, `true`, `false` or `NULL` would break
them. The kernel's build compiles every file with `-D__KERNEL__`, and that is the switch between the
two halves.

## In a kernel build: the kernel's headers {#ch-compat--kernel}

@listing src/seqlz_compat.h 16-45

`linux/build_bug.h` brings `static_assert()`, `linux/compiler.h` `__always_inline`, `noinline` and
`__cold`, `linux/math64.h` `div_u64()`, `linux/stddef.h` `sizeof_field()`, `linux/string.h` the kernel's own `memcpy()`,
`memmove()` and `memset()`, `linux/types.h` `u8` to `u64`. The rest are fallbacks for 4.14, each for
a name that arrived later: `min()` moved from `linux/kernel.h` into `linux/minmax.h` in 5.10, the
unaligned accessors from each architecture's `asm/unaligned.h` into `linux/unaligned.h` in 6.12, the
kernel's `static_assert()` came with 5.1 and `sizeof_field()` with 5.5. The kernel's `static_assert()`
lets you leave out the message and C11's `_Static_assert` does not, so every `static_assert()` in
`src/` has one. The commit that brought the header, `e814b1c`, built the codec against a current tree
with gcc, clang and arm64 clang with `W=1`, the kernel's build with extra warnings, without a warning,
and against the Mi 9T's 4.14 tree.

@listing src/seqlz_compat.h 46-54

This is the one place where the header changes a value of the kernel. `L1_CACHE_BYTES` is a cache
line as the kernel sees it, and older arm64 kernels, 4.14 among them, say 128 there, where the Mi 9T's
Cortex-A76 and A55 and mainline arm64 say 64. The decoder prefetches its tables before it decodes a
page, in `prefetch_lines()` ([chapter 12](#ch-dec-basics--prefetch)), in steps of
{{ch05.prefetch_step}} bytes with one `prefetch()` every `L1_CACHE_BYTES` inside a step:

```c
for (; q < end; q += PREFETCH_STEP)          /* PREFETCH_STEP is {{ch05.prefetch_step}} */
	for (k = 0; k < PREFETCH_STEP; k += L1_CACHE_BYTES)
		prefetch(q + k);
```

With 64 that is {{ch03.prefetch_lines}} prefetches per {{ch05.prefetch_step}} bytes, one per line.
With 128 it would be {{ch05.prefetches_128}}, at offsets 0, 128, 256 and 384, and the lines at 64,
192, 320 and 448 would never be asked for: every second line of the token table would come in late.
The test works in `#if` because the kernel defines `L1_CACHE_BYTES` as `(1 << L1_CACHE_SHIFT)`. The
`#undef` affects only the codec's files and leaves the rest of the kernel at 128; the kernel's copy,
for current kernels, does not have the block.
@assert ch05.prefetch_step == 512

## Outside the kernel: the same names from the compiler {#ch-compat--userspace}

@listing src/seqlz_compat.h 55-76

Outside the kernel the header relies on the **freestanding** headers, which come with gcc and clang
themselves, not with glibc. `string.h` and `errno.h` are part of the C library, and most builds have
it. But the benchmarks also compile the codec, and `lz4`, `lzo` and `zstd` from a kernel tree, the way
the kernel compiles them (`cmake/kernel_codecs.cmake`): with the kernel's flags, `-mno-sse` on x86-64
among them, and `-nostdinc`, which leaves only the compiler's own headers. Then `__has_include` is 0,
and the header declares the three functions the codec calls; the linker still finds them in the C
library. With userspace flags the compiler would vectorize the codec's loops with SSE registers, which
it may not do in the kernel, and the benchmarks would not measure what the kernel runs
([`docs/plan.md`](https://github.com/martinus/quetschn/blob/main/docs/plan.md#51-codecs-under-test)).

@listing src/seqlz_compat.h 78-102

The kernel's `u64` is `unsigned long long` on every architecture, glibc's `uint64_t` is `unsigned
long` on 64-bit Linux. Defining `u64` the kernel's way gives userspace the same types the kernel code
sees, e.g. for `__builtin_ctzll()` and for `printf()` formats. The codec marks
{{ch05.inline_c}} small helpers in its `.c` files and `page_lz.h` `__always_inline`, e.g. `enc_put()`,
and {{ch05.inline_compat}} more in this header, because a call in a loop that runs once per sequence
or per literal costs more than the helper's work; glibc's `sys/cdefs.h` has its own
`__always_inline`, which means the same. `noinline` and `__cold` are only for C: in C++ the macros
would also replace the words inside `[[gnu::noinline]]` and `[[gnu::cold]]`, which gcc 16 then
ignores with a warning. The decoder uses both on `lit_load_tail()`, which runs only at the end of a
literal stream, and `noinline` on `decode_literals()` ([chapter 13](#ch-literals-dec)).
`static_assert` is a keyword in C23 and in C++, so the macro is only for C before C23. Line 102 says
that cache lines have 64 bytes, true for every x86-64 CPU and for the arm64 cores that were measured;
it decides only how many prefetches `prefetch_lines()` sends, never a result.

## prefetch(): the instruction, also without SSE {#ch-compat--prefetch}

@listing src/seqlz_compat.h 104-118

This is the one function in `src/` with inline assembly, and only in userspace. The kernel builds
x86-64 with `-mno-sse`, and the x86 instruction `prefetcht0` came with SSE, although it uses no SSE
register. gcc still emits it for `__builtin_prefetch()` with `-mno-sse`; clang emits nothing, without
a warning. On x86-64 the kernel's `prefetch()` is `__builtin_prefetch()`, so a kernel built with clang
has none of the codec's prefetches. In the kernel VM that made `seqlz-2`'s cold reads at p99
24% slower with clang than with gcc, on pages a desktop swapped into zram (Ryzen 9 7950X at 4.5 GHz,
gcc 16.2.1 and clang 22.1.8,
[`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-kernel-built-with-clang-dropped-the-decoders-prefetches-fixed-p99-back-at-gccs)).

::: try What clang makes of a prefetch without SSE
```c
#include "seqlz_compat.h"
void pf(const u8 *p)  { prefetch(p + 64); }
void pfb(const u8 *p) { __builtin_prefetch(p + 64); }
```
`clang -O2 -mno-sse -Isrc -S -o - pf.c` gives `prefetcht0 64(%rdi)` for `pf()` and nothing but `retq`
for `pfb()`. gcc emits `prefetcht0` for both.
:::

The fix belongs to the kernel: `tools/kernel-port/x86-prefetcht0.patch` gives x86-64 a `prefetch()`
of its own, `prefetcht0` in inline assembly, as 32-bit x86 already had, and `port.py` applies it to
the tree it writes into. Its commit message has the measurement: with clang 22.1.8 in the kernel VM,
decompression took 2086 to 2155 ns per page without the patch and 1978 to 1988 ns with it, and a whole
swap-in 3887 to 3950 ns against 3761 to 3775 ns, three boots each
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernels-prefetch-on-x86-64-fixed-in-the-kernel-a-patch-gives-clang-its-prefetches-back)).
Line 114 is the same instruction for the benchmarks that build with the kernel's flags, so they
measure what a patched kernel runs. The `"m"` operand lets the compiler fold the offset of
`prefetch_lines()` into the addressing mode, e.g. `64(%rdi)`; nothing is loaded at C level, and an
`asm` without outputs is volatile, so it stays. `cmake/check_prefetch.cmake`, run by `ctest`,
disassembles `seqlz_decompress.c` from the build with the kernel's flags and fails without a
`prefetcht0`.

::: measured The kernel's prefetch() on arm64 costs a little on the A55
arm64's `prefetch()` in the kernel is inline assembly too, with a `"p"` operand, an address instead of
a memory operand. clang then computes each line's address with an `add` instead of an offset inside
the `prfm` instruction. On the Mi 9T's A55 that cost 0.1 to 0.3 µs per warm read, on the A76 nothing
(NDK r21e's clang 9, the phone's own 4.14 kernel). It was taken anyway, for using the kernel's
`prefetch()` instead of assembly of the codec's own
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).
:::

## min() as a function, and div_u64() {#ch-compat--min}

@listing src/seqlz_compat.h 120-124

A function-like macro `min` would expand inside `std::min(a, b)` in the C++ files. The function takes
`unsigned int`, which is all its callers need: `seqlz_token()` in `seqlz.h`
([chapter 6](#ch-header)) caps `ll` at {{ch05.ll_cap}} and `ml - 4` at {{ch05.ml_cap}} with it.

@listing src/seqlz_compat.h 126-133

`div_u64()` is the kernel's name for the one division of 64 bits the codec has, in `own_lengths()`
([chapter 11](#ch-literals-enc--own)). On 32-bit arm and x86 the compiler turns a `u64 / u32` into a
call of a helper of its runtime library, `__aeabi_uldivmod` or `__udivdi3`, and the kernel has neither:
the module would not link. The kernel's `div_u64()` divides with what the CPU has; outside the kernel
the plain division is fine. `tools/kernel-port/check.sh` looks for calls of those helpers in the
objects of `lib/seqlz/`, because its builds do not link.

## Little and big endian at any address {#ch-compat--endian}

The `u16` at the start of a page and the stream sizes are little endian
([the layout](#ch-format--layout)), the bitstreams are read most significant bit first, and the code
needs both kinds of loads, from any address, on any CPU ([chapter 3](#ch-cpu--endian)).

@listing src/seqlz_compat.h 135-163

On a little-endian CPU a little-endian value needs nothing and a big-endian one a
`__builtin_bswap64()`, one instruction, `bswap` on x86-64 and `rev` on arm64; on a big-endian CPU it
is the other way round. Nothing in the codec reads a big-endian number of 16 or 32 bits. The test in
line 139 relies on gcc and clang defining both macros: a compiler with neither would see `0 == 0` and
take the big-endian branch on any CPU.

@listing src/seqlz_compat.h 165-203

`memcpy()` into a local is the defined way to read a `u64` from any address of a byte array, and with
a constant size gcc and clang make it one load, which is also why each accessor is `__always_inline`:
the `memcpy()` has to see the caller's address. gcc 16.2.1 for x86-64 and clang 22.1.8 for arm64,
both with `-O2`:

```text
get_unaligned_le64(p + 3)    x86-64: movq 3(%rdi), %rax        arm64: ldur x0, [x0, #3]
get_unaligned_be64(p + 3)    x86-64: movq 3(%rdi), %rax        arm64: ldur x8, [x0, #3]
                                     bswap %rax                       rev  x0, x8
get_unaligned_le16(p + 1)    x86-64: movzwl 1(%rdi), %eax      arm64: ldurh w0, [x0, #1]
```

On s390x clang compiles `get_unaligned_le64()` to `lrvg`, a load that reverses the bytes on the way,
and `get_unaligned_be64()` to a plain `lg`. The kernel's own accessors use `__builtin_memcpy()` the
same way (`include/vdso/unaligned.h` of a current tree). Until 5th October 2026 the bit reader, the
matcher's hash and compare, and the copy of short offsets loaded 4 and 8 bytes in the CPU's order,
and on s390x under qemu every page failed to round trip, 4656 of 4656. Commit `91fa53e` routed every
load through accessors like these, and since then `test/seqlz_endian.c` compresses
{{ch05.endian_pages}} generated pages, hashes the output, and CI compares that hash between x86-64,
32-bit x86 and s390x.

| accessor | used by | why that order |
| --- | --- | --- |
| `get_unaligned_le16()`, `put_unaligned_le16()` | the `u16` at the start of a page, in `seqlz_decode()` and the encoder | the format says little endian |
| `get_unaligned_le32()` | the matcher's 4-byte compare of a candidate, the stream sizes of coded literals | any fixed order works for a compare; the sizes are little endian |
| `get_unaligned_le64()` | the matcher's hash and match length, the decoder's pattern for offsets below 8 | the byte at `p` in the low bits, so that bit tricks find the first byte that differs at the bottom |
| `get_unaligned_be64()` | the decoder's bit reader for the sequences, the literal streams | the bitstreams are most significant bit first |
| `put_unaligned_le64()` | the encoder's flush of its bits, the literal coder, the decoder's pattern copy | see [chapter 10](#ch-encoder) |
| `put_unaligned_be64()` | the encoder's final copy of the bitstream | turns 8 bytes around |

The pair in the encoder's final copy is the one trick here. The encoder writes the bitstream
backwards, from the end of the output buffer down, and at the end copies it the right way round behind
the literals ([chapter 10](#ch-encoder)). Byte `k` of the bitstream is then at `end - 1 - k`, and the
copy does 8 bytes at a time with

```c
put_unaligned_be64(get_unaligned_le64(end - 8 - k), out + k);
```

The little-endian load puts the byte at `end - 8 - k` into the low bits and the byte at `end - 1 - k`
into the high bits. The big-endian store writes the high bits first, so `out[k]` gets the byte from
`end - 1 - k`. On x86-64 that is a load, a `bswap` and a store: 8 bytes reversed without a loop, the
same on every CPU.
