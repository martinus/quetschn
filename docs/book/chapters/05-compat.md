# seqlz_compat.h: the kernel's names, outside the kernel {#ch-compat}

Every file of `src/` is written as if it were already in the kernel. It says `u8` and `u64`, not
`uint8_t` and `uint64_t`. It reads a 64-bit number from any address with `get_unaligned_le64()`, asks
for a cache line with `prefetch()`, checks sizes at compile time with `static_assert()` and
`sizeof_field()`, and takes the smaller of two numbers with `min()`. These are the kernel's names. The
kernel's own code uses them everywhere, and a reviewer of `lib/` expects them.

The same files also have to build outside the kernel: the tests, the benchmarks, the fuzzers and the
tools of the repository are ordinary programs. `src/seqlz_compat.h` is what makes both work. In a
kernel build it includes the kernel's headers, and the names come from there. Everywhere else it
defines the same names itself, from the few headers a C compiler brings along. The codec never sees
the difference, and that is the point: when `tools/kernel-port/port.py` writes the kernel's copy of
seqlz, it replaces the line `#include "seqlz_compat.h"` with the kernel's headers, and nothing else in
the code has to change.

There are three ways `src/` gets compiled, and this header serves two of them:

| build | who does it | where the names come from |
| --- | --- | --- |
| the kernel's copy | `tools/kernel-port/port.py` writes `lib/seqlz/` into a kernel tree | the kernel's headers; `seqlz_compat.h` is not in the copy |
| `src/` as it is, inside a kernel | `tools/zram-vm/run.sh` (the kernel VM) and `tools/zram-phone/build.sh` (a kernel module, code loaded into a running kernel, for the Mi 9T's 4.14; [chapter 15](#ch-zram-glue--init)) copy `src/` into a kernel tree | the first half of this header, `__KERNEL__` is defined |
| userspace | tests, benchmarks, fuzzers, tools, built with CMake | the second half of this header |

The middle row is why the first half has fallbacks for old kernels: the phone runs Linux 4.14, from
2017, and some names arrived later.

## The licence, the guard, and the promise {#ch-compat--top}

@listing src/seqlz_compat.h 1-14

Line 1 is the licence, as an SPDX identifier: a short, machine-readable name from a fixed list,
which the kernel requires as the first line of every file. `MIT OR GPL-2.0-only` means the user picks
one of the two. The kernel needs GPL-2.0 compatibility; the MIT half lets userspace programs and other
operating systems use the code too, which the project's plan names as the way to build a "this has
users" argument, and which is how `zstd` is licensed as well
([`docs/plan.md`](https://github.com/martinus/quetschn/blob/main/docs/plan.md#project-constraints)).
The comment style follows the kernel's rule: `/* */` in headers, `//` in `.c` files. `port.py` writes
the kernel's copy with the two licences the other way round, `GPL-2.0-only OR MIT`, which its
docstring calls the kernel's order.

Lines 2 and 3 are an include guard. If two headers both include this one, the second `#include` finds
`SEQLZ_COMPAT_H` defined and skips everything up to the `#endif` in line 193.

The comment in lines 5 to 14 lists what the header provides, and names two files of the kernel's
`lib/` that the kernel's own `tools/` build outside the kernel too: `lib/bootconfig.c`, which the
userspace tool in `tools/bootconfig/` compiles, and whose `#ifdef __KERNEL__` picks the kernel's
headers only in a kernel build; and `lib/rbtree.c`, which the tests in `tools/testing/` compile against
the stand-in headers of `tools/include/`. So the pattern has precedent in the place where seqlz wants
to go. The comment named `lib/decompress_unxz.c` until 11th October; nothing in `tools/` builds it,
it is shared with the decompressor that unpacks the kernel at boot, a different case (#183).

Lines 12 and 13 make a promise that shapes the rest of the file: nothing here defines `bool`, `true`,
`false` or `NULL`. In C++ these are keywords or come from the standard library, and a macro of the same
name would break any C++ file that includes `seqlz.h`. The test files and the benchmarks are C++
(`test/seqlz_test.cpp`, `bench/*.cpp`); they wrap the include in `extern "C" { }` and otherwise use the
header as it is.

::: kernel What `__KERNEL__` is
The kernel's build system compiles every file with `-D__KERNEL__`. Headers that are shared between the
kernel and userspace, e.g. the ones under `include/uapi/` that describe system calls, test it to leave
out what userspace must not see. For seqlz it is the switch between the two halves of this file: a
kernel build gets the kernel's headers, everything else gets the definitions below.
:::

## In a kernel build: the kernel's headers {#ch-compat--kernel}

@listing src/seqlz_compat.h 16-30

Each header brings a few of the names the codec uses:

- `linux/build_bug.h`: `static_assert()` and `BUILD_BUG_ON()`, checks that the compiler does and that
  cost nothing at run time.
- `linux/cache.h`: `L1_CACHE_BYTES`, the size of a cache line, through `asm/cache.h` of the
  architecture.
- `linux/compiler.h`: the attributes `__always_inline`, `noinline` and `__cold`.
- `linux/errno.h`: `EINVAL`, the error number that `seqlz_decode()` and `seqlz_tables_init()` return
  as `-EINVAL`.
- `linux/minmax.h`, or `linux/kernel.h` before 5.10: `min()`.
- `linux/prefetch.h`: `prefetch()`.
- `linux/stddef.h`: `NULL`, `true`, `false`, `offsetof()` and `sizeof_field()`.
- `linux/string.h`: `memcpy()`, `memmove()` and `memset()`, the kernel's own, because the kernel has
  no C library.
- `linux/types.h`: `u8`, `u16`, `u32`, `u64`, `size_t` and `bool`.

Lines 21 to 26 are the first fallback for old kernels. `min()` and `max()` moved out of the large
`linux/kernel.h` into a header of their own, `linux/minmax.h`, in Linux 5.10. `__has_include(<...>)` is
a preprocessor operator that is 1 when the header can be found and 0 when not; gcc since version 5 and
clang have it, and C23 made it standard. On a current kernel the codec includes the small header; on
the phone's 4.14 it falls back to `linux/kernel.h`, which has `min()` there.

@listing src/seqlz_compat.h 31-44

The same trick for the unaligned accessors (lines 31 to 36). Since Linux 6.12 they are in
`linux/unaligned.h`; before, every architecture had its own `asm/unaligned.h`, and 4.14 is one of
those. The commit that brought this header, `e814b1c`, says the codec built against the Mi 9T's 4.14
tree with it, and against a current tree with gcc, clang and arm64 clang, there with `W=1`, the
kernel's build with extra compiler warnings ([chapter 15](#ch-zram-glue--check)), and no warnings.

Lines 37 to 40 give 4.14 a `static_assert`. The kernel's macro arrived in 5.1. It wraps C11's keyword
`_Static_assert(expression, "message")`, which every compiler the kernel supports has, so where the
macro is missing the keyword does the same job. One difference matters: the kernel's macro lets you
leave out the message, `_Static_assert` in C11 does not. Every `static_assert()` in `src/` has a
message, so both spellings compile.

Lines 41 to 44 do the same for `sizeof_field()`, which came with 5.5. `sizeof_field(TYPE, MEMBER)`
is the size of one member of a struct, without an object of it: `((TYPE *)0)->MEMBER` names the
member of a struct at address 0, and `sizeof` only looks at its type and never evaluates the
expression, so nothing is read from address 0. The definition is the kernel's own, character for
character. The codec uses it in `static_assert`s, e.g. that the matcher's hash table has room for a
page of literals (`seqlz_compress.c`, see [chapter 11](#ch-literals-enc)) and that each decode table
is a multiple of {{ch05.prefetch_step}} bytes for the prefetch loop (`seqlz_decompress.c`, see
[chapter 12](#ch-dec-basics)).

@listing src/seqlz_compat.h 45-53

This is the one place where the header changes a value of the kernel instead of only filling a gap.
`L1_CACHE_BYTES` is the size of a cache line as the kernel sees it. Older arm64 kernels, 4.14 among
them, say 128 there. The Cortex-A76 and A55 of the Mi 9T have lines of 64 bytes, and mainline arm64
says 64 too.

The codec's decoder asks for its tables before it decodes a page, with a **prefetch** for each of
their 64-byte cache lines ([chapter 3](#ch-cpu--prefetch)), in `prefetch_lines()` ([chapter
12](#ch-dec-basics--prefetch)). It walks the table in steps of {{ch05.prefetch_step}} bytes and,
inside each step, calls `prefetch()` every `L1_CACHE_BYTES` bytes:

```c
for (; q < end; q += PREFETCH_STEP)          /* PREFETCH_STEP is {{ch05.prefetch_step}} */
	for (k = 0; k < PREFETCH_STEP; k += L1_CACHE_BYTES)
		prefetch(q + k);
```

With 64 that is {{ch03.prefetch_lines}} prefetches per {{ch05.prefetch_step}} bytes, one per line.
With 128 it would be {{ch05.prefetches_128}}, at offsets 0, 128, 256 and 384, and the lines at 64,
192, 320 and 448 would never be asked for: every second line of the token table would come in late.
So lines 50 to 53 say 64 again when an arm64 kernel says 128. The test `L1_CACHE_BYTES == 128` works
in `#if` because the kernel defines it as `(1 << L1_CACHE_SHIFT)`, an expression of numbers that the
preprocessor can evaluate.
@assert ch05.prefetch_step == 512

The `#undef` affects only the files of the codec, which include this header, and only below this line.
The rest of the kernel keeps its 128. In the kernel's copy this block is gone, because the copy is for
current kernels, where arm64 has 64.

## Outside the kernel: the same names from the compiler {#ch-compat--userspace}

@listing src/seqlz_compat.h 54-75

Line 54 starts the half for everything that is not a kernel. It may only rely on the headers a C
compiler has to provide even without a C library, the **freestanding** headers. `stdbool.h` gives
`bool`, `true` and `false`; `stddef.h` gives `size_t`, `NULL` and `offsetof()`. Both come with gcc
and clang themselves, not with glibc.

`string.h` and `errno.h` are not freestanding: they are part of the C library. Most builds have it,
and then lines 65 and 72 include the real headers. But the benchmarks also compile the codec, and
`lz4`, `lzo` and `zstd` from a kernel tree, the way the kernel compiles them
(`cmake/kernel_codecs.cmake`): the kernel's compiler flags, `-mno-sse` on x86-64 included, and
`-nostdinc`, which removes the C library's headers from the search path. Only the compiler's own
directory is added back. Then `__has_include(<string.h>)` is 0, and the header declares the three
functions the codec calls itself (lines 67 to 69). They are only declarations; the linker finds the
definitions in the C library as usual. And `EINVAL` is 22 (line 74), its value in Linux.

The point of that build is a fair comparison. Compiled with userspace flags, the compiler would
vectorize loops of the codec with SSE registers, which it may not do in the kernel, and the numbers
would not be the kernel's ([`docs/plan.md`](https://github.com/martinus/quetschn/blob/main/docs/plan.md#51-codecs-under-test)).

@listing src/seqlz_compat.h 77-91

Lines 78 to 81 are the kernel's integer types. The comment says "as the kernel has them on every
architecture, u64 too": the kernel's `u64` is `unsigned long long` everywhere, while glibc's
`uint64_t` is `unsigned long` on 64-bit Linux. Both are 64 bits, but they are different types to the
compiler. `printf()` needs `%llu` for one and `%lu` for the other, and `__builtin_ctzll()`, which the
codec uses, takes an `unsigned long long`. Defining `u64` the kernel's way gives userspace the same
types the kernel code sees. `u16` as `unsigned short` and `u32` as `unsigned int` assume 16 and 32 bits,
which holds on every ABI Linux runs on.

Lines 84 to 86 define `__always_inline`. `inline` alone is a hint that the compiler may ignore; with
`__attribute__((__always_inline__))` gcc and clang inline the function at every call or fail with an
error. The codec marks its small helpers this way, {{ch05.inline_c}} of them in its `.c` files and
`page_lz.h`, e.g. `enc_put()` in the encoder, and {{ch05.inline_compat}} more in this header,
`prefetch()` and the unaligned accessors below, because a call in a loop that runs once per sequence
or per literal would cost more than the helper's work. The `#ifndef` is there because glibc defines
`__always_inline` in `sys/cdefs.h`, which `string.h` pulls in; glibc's version means the same thing.

Lines 88 to 91 define `noinline` and `__cold`, but only for C. `noinline` keeps a function out of its
callers. `__cold` tells the compiler that a function is rarely called: it optimizes it for size, puts
it into a separate section away from the hot code, and treats the branches that lead to it as unlikely.
The decoder uses both on `lit_load_tail()`, which loads the last bytes of a literal stream and runs only
at the end of a page, and `noinline` on `decode_literals()` ([chapter 13](#ch-literals-dec)).

Why only for C: in C++ these words also appear inside standard attributes, `[[gnu::noinline]]` and
`[[gnu::cold]]`. A macro does not know about attributes and replaces the word there too. With gcc 16 the
result is not even an error, only a warning that the attribute `gnu::attribute` is unknown and ignored,
and the function is no longer kept out of line. No C++ file in the repository writes such an attribute
today; the `#ifndef __cplusplus` keeps it that way for any that does, since only the codec's `.c` files
need the macros.

@listing src/seqlz_compat.h 93-101

Lines 94 to 97 give C11 its `static_assert`. In C11 the keyword is `_Static_assert`, and the name
`static_assert` is a macro from `assert.h`. In C23 and in C++, `static_assert` is a keyword, and a
macro of that name is wrong or useless there. So the macro is defined only in C, only before C23 (the
value of `__STDC_VERSION__` for C23 is `202311L`), and only if `assert.h` has not defined it already.
The test `!defined(__STDC_VERSION__)` covers a C compiler so old that it does not say its version.

Line 99 is `sizeof_field()`, the kernel's definition again; see above for how it works. Line 101 says
that cache lines have 64 bytes. That is true for every x86-64 CPU and for the arm64 cores that were
measured; it decides only how many prefetches `prefetch_lines()` sends, never a result.

## prefetch(): the instruction, also without SSE {#ch-compat--prefetch}

@listing src/seqlz_compat.h 103-117

This is the one function in `src/` that contains inline assembly, and only in userspace. Its story
starts in the kernel.

The kernel builds x86-64 with `-mno-sse`: kernel code may not touch the SSE registers, which hold the
interrupted program's values, unless it saves them first with `kernel_fpu_begin()`, and that costs
time. The x86 instruction `prefetcht0` came with SSE, the
Pentium III's extension, although it uses no SSE register. gcc still emits `prefetcht0` for
`__builtin_prefetch()` with `-mno-sse`; clang emits nothing, without a warning. On x86-64 the kernel's
`prefetch()` is `__builtin_prefetch()`, so a kernel built with clang has none of the codec's
prefetches. In the kernel VM this made `seqlz-fast-lit`'s cold reads at p99 22% and 26% slower with
clang than with gcc, on two desktop dumps (Ryzen 9 7950X at 4.5 GHz, gcc 16.2.1 and clang 22.1.8,
[`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-kernel-built-with-clang-dropped-the-decoders-prefetches-fixed-p99-back-at-gccs)).

You can see it with the header itself:

::: try What clang makes of a prefetch without SSE
```c
#include "seqlz_compat.h"
void pf(const u8 *p)  { prefetch(p + 64); }
void pfb(const u8 *p) { __builtin_prefetch(p + 64); }
```
`clang -O2 -mno-sse -Isrc -S -o - pf.c` gives `prefetcht0 64(%rdi)` for `pf()` and nothing but `retq`
for `pfb()`. gcc emits `prefetcht0` for both.
:::

The fix belongs to the kernel, not to the codec: `tools/kernel-port/x86-prefetcht0.patch` changes
`arch/x86/include/asm/processor.h` so that x86-64 has a `prefetch()` of its own, `prefetcht0` in inline
assembly, as 32-bit x86 already had one. `port.py` applies it to the tree it writes into. Its commit
message has the measurement: with clang 22.1.8 in the kernel VM, decompression took 2086 to 2155 ns per
page without the patch and 1978 to 1988 ns with it, and a whole swap-in 3887 to 3950 ns against 3761 to
3775 ns, three boots each. The codec's own files contain no assembly; the project's rule is pure C
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernels-prefetch-on-x86-64-fixed-in-the-kernel-a-patch-gives-clang-its-prefetches-back)).

Lines 110 to 117 are that patched `prefetch()` for userspace. When is userspace x86-64 without SSE?
Only in the benchmark build with the kernel's flags described above. There line 113 is used, so the
benchmarks measure what a patched kernel runs; everywhere else, line 115, the compiler's builtin.

Line 113 in detail. `__asm__("prefetcht0 %0" : : "m"(*(const char *)p))` has no outputs (the first
`:`) and one input (the second): `"m"` means "a memory operand", and `*(const char *)p` names the byte
at `p`. The compiler fills `%0` with an addressing mode for it, e.g. `64(%rdi)`, and folds an offset
like the `+ k` of `prefetch_lines()` into it. Nothing is loaded at C level: the dereference only says
which address. An `asm` without outputs is treated as `volatile` by gcc and clang, so it is not removed
even though it has no visible result. A prefetch never faults, also for an address that is not mapped.

A test checks that the prefetches stay: `cmake/check_prefetch.cmake`, run by `ctest`, disassembles
`seqlz_decompress.c` from the build with the kernel's flags and fails if there is no `prefetcht0`.

::: measured The kernel's prefetch() on arm64 costs a little on the A55
arm64's `prefetch()` in the kernel is inline assembly too, with a `"p"` operand, an address instead of
a memory operand. clang then computes each line's address with an `add` instead of an offset inside
the `prfm` instruction. On the Mi 9T's A55 that cost 0.1 to 0.3 µs per warm read, on the A76 nothing
(NDK r21e's clang 9, the phone's own 4.14 kernel). It was taken anyway, for using the kernel's
`prefetch()` instead of assembly of the codec's own
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).
:::

## min() as a function {#ch-compat--min}

@listing src/seqlz_compat.h 119-123

The kernel's `min(x, y)` is a macro. It checks that both arguments have compatible types and evaluates
each once. C++ has `std::min`, and a function-like macro called `min` would expand inside
`std::min(a, b)` too, which then fails to compile with "expected unqualified-id". A function of the
same name does not interfere: `std::min` is a different name in a different namespace.

The function takes `unsigned int`, which is all the codec needs: its only callers are in
`seqlz_token()` in `seqlz.h` ([chapter 6](#ch-header)), which caps `ll` at {{ch05.ll_cap}} and
`ml - 4` at {{ch05.ml_cap}}. A function evaluates its arguments once, as the kernel's macro does.
gcc compiles it to a compare and a conditional move, without a branch.

## Little and big endian at any address {#ch-compat--endian}

The format is little endian: the `u16` at the start of a page is low byte first, and so are the stream
sizes in the header of coded literals ([the layout](#ch-format--layout)). The bitstreams, however, are
read most significant bit first: the first bit of a stream is bit 7 of its first byte. The code needs
both kinds of loads, from any address, on any CPU. Two words first.

::: cpu Endianness and unaligned access
A CPU that loads 8 bytes into a 64-bit register has to decide which byte goes where. **Little
endian** CPUs, x86-64 and arm64 as Linux runs them, put the byte at the lowest address into the lowest
bits of the register. **Big endian** CPUs, e.g. IBM's s390x, put it into the highest bits. A load is
**unaligned** when its address is not a multiple of its size, e.g. 8 bytes from address 3. x86-64 and
arm64 load unaligned data as fast as aligned data in most cases; some older CPUs fault on it.
:::

@listing src/seqlz_compat.h 125-139

Lines 129 to 139 define four macros that turn a number between the CPU's order and a fixed one.
`__BYTE_ORDER__` and `__ORDER_BIG_ENDIAN__` are predefined by gcc and clang. On a little-endian CPU a
little-endian value needs nothing (lines 135 to 137) and a big-endian one needs its bytes reversed,
`__builtin_bswap64()`, a single instruction (`bswap` on x86-64, `rev` on arm64). On a big-endian CPU it
is the other way round (lines 130 to 133). The 16- and 32-bit versions exist only for little endian,
because nothing in the codec reads a big-endian number of those sizes.

::: warn The test in line 129 relies on gcc and clang
In `#if`, a name that is not a macro counts as 0. A compiler that defines neither `__BYTE_ORDER__` nor
`__ORDER_BIG_ENDIAN__` would see `0 == 0` and take the big-endian branch on any CPU. gcc and clang
define both, and the kernel builds only with those two.
:::

@listing src/seqlz_compat.h 141-169

These are the kernel's accessors, written for userspace. Each copies the bytes into a local variable
with `memcpy()`, then converts the order. Why not simply `*(const u64 *)p`?

- If `p` is not a multiple of 8, the C standard says the cast pointer is undefined behaviour, and the
  compiler may assume the address is aligned. UBSan's alignment check reports it
  ([chapter 3](#ch-cpu--ubsan)), and some CPUs fault.
- The data is an array of `u8`. Reading it through a `u64 *` breaks C's aliasing rule, which lets the
  compiler assume that a `u64 *` and a `u8` array are different objects and reorder reads and writes
  between them. The kernel compiles with `-fno-strict-aliasing`, userspace does not.

`memcpy()` into a local has neither problem: copying bytes is always allowed, at any address. And it
costs nothing. Since the size is a constant, gcc and clang do not call `memcpy()`; they turn it into one
load:

```text
get_unaligned_le64(p + 3)    x86-64: movq 3(%rdi), %rax        arm64: ldur x0, [x0, #3]
get_unaligned_be64(p + 3)    x86-64: movq 3(%rdi), %rax        arm64: ldur x8, [x0, #3]
                                     bswap %rax                       rev  x0, x8
get_unaligned_le16(p + 1)    x86-64: movzwl 1(%rdi), %eax      arm64: ldurh w0, [x0, #1]
```

That is gcc 16 for x86-64 and clang 22 for arm64, both with `-O2`. On s390x clang compiles
`get_unaligned_le64()` to `lrvg`, a load that reverses the bytes on the way, and
`get_unaligned_be64()` to a plain `lg`. The kernel's own accessors do the same with
`__builtin_memcpy()` (in `include/vdso/unaligned.h` of a current tree), whose comment gives the same
reason: no type punning.

`static __always_inline` on each accessor makes sure it is inlined, so that the `memcpy()` sees a
constant size and the address of the caller. Without inlining, it would be a call per load.

The codec did not always do it this way. Until 5th October 2026 the bit reader, the matcher's hash
and compare, and the copy of short offsets loaded 4 and 8 bytes in the CPU's order. On s390x under
qemu every page failed to round trip, 4656 of 4656. Commit `91fa53e` routed every load through
accessors like these, and since then `test/seqlz_endian.c` compresses {{ch05.endian_pages}}
generated pages, hashes the output, and CI compares that hash between x86-64, 32-bit x86 and s390x.

@listing src/seqlz_compat.h 171-193

The big-endian load and the two 64-bit stores complete the set. Who uses which:

| accessor | used by | why that order |
| --- | --- | --- |
| `get_unaligned_le16()`, `put_unaligned_le16()` | the `u16` at the start of a page, in `seqlz_decode()` and the encoder | the format says little endian |
| `get_unaligned_le32()` | the matcher's 4-byte compare of a candidate, the stream sizes of coded literals | any fixed order works for a compare; the sizes are little endian |
| `get_unaligned_le64()` | the matcher's hash and match length, the decoder's pattern for offsets below 8 | the byte at `p` in the low bits, so that bit tricks find the first byte that differs at the bottom |
| `get_unaligned_be64()` | the decoder's bit reader for the sequences, the literal streams | the bitstreams are most significant bit first |
| `put_unaligned_le64()` | the encoder's flush of its bits, the literal coder, the decoder's pattern copy | see [chapter 10](#ch-encoder) |
| `put_unaligned_be64()` | the encoder's final copy of the bitstream | turns 8 bytes around |

The big-endian load is the reason the bit reader can be short. A bitstream that is read most
significant bit first has its first bit in bit 7 of its first byte. `get_unaligned_be64()` puts that
first byte into bits 63 to 56 of the register, the second byte below it, and so on. The next code is
then simply the top bits of the register, `bits >> (64 - 11)` for an 11-bit lookup:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 210" role="img" aria-label="The bytes 01 to 08 in memory, loaded as little endian and as big endian into a 64-bit register">
  <g font-size="12">
    <text x="10" y="44">memory, p[0] to p[7]</text>
    <text x="10" y="114">get_unaligned_le64(p)</text>
    <text x="10" y="184">get_unaligned_be64(p)</text>
    <text x="200" y="88" font-size="11">bit 63</text>
    <text x="660" y="88" font-size="11">bit 0</text>
    <text x="200" y="158" font-size="11">bit 63</text>
    <text x="660" y="158" font-size="11">bit 0</text>
  </g>
  <g font-size="13" text-anchor="middle">
    <rect x="200" y="26" width="56" height="26" class="fill-acc" stroke="currentColor"/>
    <text x="228" y="44" class="on-fill">01</text>
    <rect x="260" y="26" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="288" y="44">02</text>
    <rect x="320" y="26" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="348" y="44">03</text>
    <rect x="380" y="26" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="408" y="44">04</text>
    <rect x="440" y="26" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="468" y="44">05</text>
    <rect x="500" y="26" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="528" y="44">06</text>
    <rect x="560" y="26" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="588" y="44">07</text>
    <rect x="620" y="26" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="648" y="44">08</text>

    <rect x="200" y="96" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="228" y="114">08</text>
    <rect x="260" y="96" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="288" y="114">07</text>
    <rect x="320" y="96" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="348" y="114">06</text>
    <rect x="380" y="96" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="408" y="114">05</text>
    <rect x="440" y="96" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="468" y="114">04</text>
    <rect x="500" y="96" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="528" y="114">03</text>
    <rect x="560" y="96" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="588" y="114">02</text>
    <rect x="620" y="96" width="56" height="26" class="fill-acc" stroke="currentColor"/><text x="648" y="114" class="on-fill">01</text>

    <rect x="200" y="166" width="56" height="26" class="fill-acc" stroke="currentColor"/><text x="228" y="184" class="on-fill">01</text>
    <rect x="260" y="166" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="288" y="184">02</text>
    <rect x="320" y="166" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="348" y="184">03</text>
    <rect x="380" y="166" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="408" y="184">04</text>
    <rect x="440" y="166" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="468" y="184">05</text>
    <rect x="500" y="166" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="528" y="184">06</text>
    <rect x="560" y="166" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="588" y="184">07</text>
    <rect x="620" y="166" width="56" height="26" class="fill-sheet" stroke="currentColor"/><text x="648" y="184">08</text>
  </g>
</svg>
</div>
<figcaption>The same 8 bytes loaded both ways: little endian puts p[0] into the lowest bits, big endian into the highest, where a bit reader that reads most significant bit first wants it.</figcaption>
</figure>

The pair in the encoder's final copy is worth a look. The encoder writes the bitstream backwards, from
the end of the output buffer down, and at the end copies it the right way round behind the literals
([chapter 10](#ch-encoder)). Byte `k` of the bitstream is then at `end - 1 - k`, and the copy does 8
bytes at a time with

```c
put_unaligned_be64(get_unaligned_le64(end - 8 - k), out + k);
```

The little-endian load puts the byte at `end - 8 - k` into the low bits and the byte at `end - 1 - k`
into the high bits. The big-endian store writes the high bits first, so `out[k]` gets the byte from
`end - 1 - k`, exactly as needed. On x86-64 this is a load, a `bswap` and a store: 8 bytes reversed in
three instructions, on any CPU, without a loop.

Lines 191 and 193 close the `#ifdef __KERNEL__` of line 16 and the include guard of line 2.
