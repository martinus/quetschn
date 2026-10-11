# From a library to a zram backend {#ch-zram-glue}

Everything up to here is a library: functions that take a page and tables and work memory, and know
nothing about who calls them. zram calls a compressor through a small table of function pointers, and
something has to sit between the two: keep the tables, hand out the work memory per CPU, check the
lengths, and turn seqlz's return values into zram's. `src/zram_seqlz.c` is that piece for the
project's benchmark harness. It is the last file of `src/`, 147 lines, and the only one that knows
zram exists.

The kernel does not get this file. What a kernel tree gets is written by a script,
`tools/kernel-port/port.py`, from the rest of `src/` and a few files of its own. This chapter goes
through `src/zram_seqlz.c` line by line first, then shows what changes on the way into the kernel and
why, and ends with the tests that hold all of it together: the unit tests, the fuzzers, a check that
the compressed bytes are the same on a big-endian CPU, and the CI jobs that run them.

## The harness's picture of zram {#ch-zram-glue--harness}

The harness measures `lz4`, `lzo-rle`, `zstd` and seqlz in userspace, and it wants to call each of
them the way zram calls it in the kernel. So it has a copy of zram's interface to a compressor, in
`bench/kernel_codecs/zram_codec.h`. That header is not in `src/`, so here is its essence, shortened:

```c
/* struct zcomp_params: one per zram device and algorithm */
struct quetschn_params {
	const void *dict;
	size_t dict_size;
	int level;          /* setup_params() replaces QUETSCHN_LEVEL_DEFAULT */
	unsigned page_size; /* PAGE_SIZE */
	void *drv_data;
	size_t allocated;   /* bytes the codec allocated for these params */
};

/* struct zcomp_ctx: one per CPU and direction */
struct quetschn_stream {
	void *context;
	size_t allocated;   /* bytes the codec allocated for this stream */
};

struct quetschn_codec {
	const char *name;
	int (*setup_params)(struct quetschn_params *p);
	void (*release_params)(struct quetschn_params *p);
	int (*create_cctx)(struct quetschn_params *p, struct quetschn_stream *s);
	void (*destroy_cctx)(struct quetschn_stream *s);
	int (*create_dctx)(struct quetschn_params *p, struct quetschn_stream *s);
	void (*destroy_dctx)(struct quetschn_stream *s);
	int (*compress)(struct quetschn_params *p, struct quetschn_stream *s,
			const void *src, unsigned int src_len, void *dst,
			unsigned int *dst_len);
	int (*decompress)(struct quetschn_params *p, struct quetschn_stream *s,
			  const void *src, unsigned int src_len, void *dst,
			  unsigned int *dst_len);
};
```

Each name maps to one of zram's. `struct quetschn_params` is the kernel's `struct zcomp_params`, the
settings of one zram device: a compression level, an optional dictionary, and `drv_data`, a pointer
the backend may fill with whatever it prepares from them. `struct quetschn_stream` is `struct
zcomp_ctx`, which holds one `void *context`. And `struct quetschn_codec` is `struct zcomp_ops`, the
table of function pointers each zram backend fills in. The harness adds two fields the kernel does not
have: `page_size`, because the harness runs 4 KiB and 16 KiB pages in one binary, and `allocated`,
which counts the bytes a codec holds, so that the harness can report the memory per device and per
CPU.

::: kernel How zram calls a compressor
zram keeps one **stream** per CPU and per compressor. A stream holds the compressor's context (its
work memory), a buffer of 2 pages for the compressed output, and a `struct mutex`. To compress or
decompress a page, zram takes the stream of the CPU it runs on with `zcomp_stream_get()`, which locks
that mutex, calls `ops->compress()` or `ops->decompress()`, and unlocks it again with
`zcomp_stream_put()`. So one context is never used by two pages at the same time, and the context
needs no locking of its own.

The streams are created when a CPU comes up, through `create_ctx()`. That is the time to allocate; the
compress and decompress calls themselves must not. `setup_params()` runs once per device and
algorithm, before any stream exists, and `release_params()` when the device goes away.

In October 2026 Sergey Senozhatsky posted a series that splits the one stream into a write stream
and a read stream, so that a reader does not wait for a writer on the same CPU. With it a backend has
two context functions, `create_cctx()` for compression and an optional `create_dctx()` for
decompression. The harness has both since the project followed that series
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zrams-contexts-split-into-compression-and-decompression-the-same-time-in-the-vm-the-same-memory-for-seqlz)).
:::

A codec that has no `create_dctx` gets the compression context in `decompress()`, as every zram
backend did before the split. The harness's codecs for `lz4`, `lzo` and `zstd` are copies of the
kernel's glue from before the series, so they all work that way. seqlz uses the split, and its two
codecs use it differently, as we will see.

## Two codecs, one format, no dictionary {#ch-zram-glue--head}

@listing src/zram_seqlz.c 1-9

Line 1 is the licence line every file of the project starts with ([chapter 5](#ch-compat--top)).
Lines 3 and 4 name the two codecs this file defines. Both write the
same format and both are read by the same decoder. They differ in one argument to
`seqlz_compress()`: `seqlz-fast` keeps the literals raw, `seqlz-fast-lit` lets
`code_literals()` ([chapter 11](#ch-literals-enc)) Huffman code them when that saves at least $\frac{1}{16}$ of
them plus 51 bytes. In the kernel's copy these are levels 1 and 2 of one backend called `seqlz`; the
harness names them as two codecs, so that it can compare them side by side with the others in one run.

Lines 5 and 6 are a design decision. zram can give a compressor a dictionary, a blob of bytes the
administrator loads per device. `zstd` and `lz4` use it as a set of earlier data that matches may
point into. For seqlz a dictionary could have carried other code tables, e.g. tables trained on one
phone's pages, and measured, those saved too little to pay ([chapter 8](#ch-codes--fixed)). So since commit `87a0ba4` the tables are part of the format, and the dictionary is ignored, the same
way zram's `lzo` backend ignores one. The kernel's backend goes one step further and refuses a
dictionary with an error, see [below](#ch-zram-glue--backend).

Line 8 includes `seqlz.h`, the public header of [chapter 6](#ch-header). Line 9 includes the
harness's header from above. Note what is not included: `seqlz_internal.h`. The glue uses the tables
only through a pointer to the incomplete `struct seqlz_tables` and the functions of `seqlz.h`, so it
does not depend on how the tables are laid out.

## Setting up a device: the tables, built and checked {#ch-zram-glue--setup}

@listing src/zram_seqlz.c 11-30

`setup()` is the harness's `setup_params()`. It runs once per device, before any page.

Line 15 refuses a page size the build was not made for. seqlz is compiled for one page size,
`QUETSCHN_PAGE_BITS` 12 or 14, and `SEQLZ_PAGE` is `1U << QUETSCHN_PAGE_BITS`. The harness loads a
corpus whose header names its page size, so a 4 KiB build given a corpus of 16 KiB pages fails here
with `-1`, instead of reading past each page. In the kernel this check has no place: there the page
size is `PAGE_SIZE`, and the kernel's copy refuses to build for anything but 4 KiB, see
[below](#ch-zram-glue--port).

Line 17 allocates the tables. `seqlz_tables_size()` returns `sizeof(struct seqlz_tables)`, which is
43 280 bytes for 4 KiB pages: the token table, 8 literal tables, the cost table `lit_cost` and two
tables of length values, laid out in [chapter 7](#ch-internal). The glue cannot write `sizeof`
itself, because it never sees the struct's definition; that is what the function is for.
`quetschn_zalloc()` stands in for the kernel's `kvzalloc()` ([below](#ch-zram-glue--backend)): it returns zeroed memory, aligned to a
page for blocks of a page or more, and it adds the size to `p->allocated`. So the harness reports
43 280 bytes per device for seqlz. Line 18 turns a failed allocation into `-1`, the harness's error.

Lines 20 to 27 build the tables. `seqlz_tables_init()` ([chapter 8](#ch-codes)) turns the code
lengths compiled into `seqlz_default_own` into canonical codes and decode tables, and returns
`-EINVAL` if the lengths are not a complete prefix code. `seqlz_all_symbols()` returns whether every
symbol the encoder may have to write has a code: every token or the escape, every length value, every
byte in every literal table. The decoder does not need that, a complete code may leave symbols out,
but the encoder does. The comment says what this check is for: the tables are fixed, so it can only
fail if a training run produced bad lengths, and then it fails before the first page and not in the
middle of a benchmark. On failure line 25 frees the tables again and takes their bytes off
`p->allocated`, and line 26 returns the error.

The `||` in line 24 evaluates left to right and stops at the first true operand. So
`seqlz_all_symbols()` only runs on tables that `seqlz_tables_init()` built, never on half-built ones.

Line 28 stores the tables in `drv_data`, where every later call finds them, and line 29 reports
success.

::: why One set of tables per device here, one for the whole kernel there
The harness builds the tables once per device, in `setup()`. The kernel's copy builds them once at boot into one global struct that all devices and CPUs read, see
[The tables, built once at boot](#ch-zram-glue--init). Building 43 KB of tables in `setup_params()`
for every zram device and priority, from constant data, was what the kernel copy did until commit
`aaa381e`; the review asked for the global.
:::

@listing src/zram_seqlz.c 32-42

`release()` is `release_params()`: it frees the tables and sets `drv_data` to `NULL`, so that a
second release, or a use after release, finds a null pointer and not freed memory.
`quetschn_free()` accepts `NULL`, as the kernel's `kvfree()` does.

`destroy()` frees one stream's context. It takes no params, only the stream, so it serves both
directions: both codecs use it as `destroy_cctx`, and `seqlz-fast-lit` also as `destroy_dctx`.

## The compression context is the matcher's table {#ch-zram-glue--cctx}

@listing src/zram_seqlz.c 44-53

The work memory of compression is one `struct seqlz_state` per CPU ([chapter 6](#ch-header)): a union
of the matcher's hash table, `u16 table[1U << SEQLZ_HASH_BITS]`, and a page of bytes,
`u8 literals[SEQLZ_PAGE]`. With 4 KiB pages the table has 4096 entries of 2 bytes, 8192 bytes, and
the page fits into the same 8192 bytes, so `sizeof(struct seqlz_state)` is 8192. The union is there
because `code_literals()` copies the raw literals into the hash table's memory once the matcher is
done with it ([chapter 11](#ch-literals-enc)), so coding the literals needs no memory of its own.

"For both" in the comment means for both codecs: `seqlz-fast` and `seqlz-fast-lit` compress with the
same 8192 bytes. The second sentence is the reason `seqlz-fast` has no decompression context at all,
which we see in `fast_decompress()`.

Line 50, `(void)p;`, marks the parameter as unused on purpose. With `-Wextra` a compiler warns about
an unused parameter, and a cast to `void` is the usual C way to silence that for one parameter. The
function has to take `p`, because the function pointer type in `struct quetschn_codec` has it. The
kernel builds with `-Wextra` too but turns this one warning off below `W=3`, which is why the kernel's
backend later in this chapter does without such casts.

Line 51 allocates the state, zeroed. The zeroing is not needed for correctness: the matcher clears
its table with `memset()` at the start of every page ([chapter 9](#ch-matcher)). It is what
`kvzalloc()` does in the kernel, and the harness mirrors it. Line 52 is C's way of returning 0 for a
non-null pointer and `-1` for a null one.

## seqlz-fast: compress and decompress {#ch-zram-glue--fast}

@listing src/zram_seqlz.c 55-68

`fast_compress()` is one page through `seqlz_compress()` with raw literals. Its interface is zram's:
`*dst_len` is the capacity of `dst` on the way in and the compressed length on the way out, and the
return value is 0 for success.

Line 61 checks that the input is a whole page. zram always passes `PAGE_SIZE`, so this never fails
from zram; it protects against a harness bug. Line 63 calls the compressor with the tables from
`drv_data`, the per-CPU state from `s->context`, the page, the output buffer and its capacity, and
`0` for `coded`. `seqlz_compress()` returns the length of the compressed page, or 0 if it did not fit
into `dst_cap` bytes ([chapter 10](#ch-encoder)). zram gives a compressor 2 pages of output, and the
encoder proves that a page always fits into 2 pages, so with zram's buffer that 0 never comes. With a
smaller buffer, e.g. the one page zswap gives, it can, and line 64 turns it into an error. Line 66
reports the length.

@listing src/zram_seqlz.c 70-85

`fast_decompress()` decodes a page that `seqlz-fast` wrote.

Line 78 discards `s`. As the comment above says, `s` here is the compression context: this codec has
no `create_dctx`, so the harness passes the compression context to `decompress()`, as zram did before
the split. The decoder needs no work memory for pages with raw literals, so the function does not
touch it. In zram with the split, a codec without `create_dctx` gets a stream with no context at all,
and that works too; the test `seqlz compresses with the hash table, seqlz-fast-lit decompresses with
the scratch` in `test/kernel_codecs_test.cpp` decodes every test page that way.

Line 79 is a prefetch, the one line of this file that is about speed. `quetschn_prefetch_page()` is
in the harness's header:

```c
static inline void quetschn_prefetch_page(const void *src, unsigned int src_len,
					  void *dst, unsigned int dst_len)
{
	const char *q;
	char *o;

	for (q = (const char *)src + 64; q < (const char *)src + src_len; q += 64)
		__builtin_prefetch(q);
	for (o = (char *)dst; o < (char *)dst + dst_len; o += 64)
		__builtin_prefetch(o, 1);
}
```

The first loop asks for every cache line of the compressed page from its second line on. The second
asks for every line of the 4096-byte destination, with the second argument `1`, which means "for
writing": the CPU may fetch the line in a state that lets it write without asking again.

Prefetches issued together let the misses overlap, so the CPU waits about once instead of once per line
([chapter 3](#ch-cpu--prefetch)). The CPU's own prefetcher guesses the next lines from a pattern, e.g. a
pointer walking forward, but seqlz reads a compressed page in several places at once: the literals from
the front, the bitstream from behind them, and with coded literals 8 streams more.

The prefetch of the compressed data is the one that matters. In the kernel VM, with the compressed
data flushed from the cache, it took `lz4`'s p99 read latency from 5600 to 3891 ns, and seqlz's from
5450 to 4410 ns; the destination prefetch helped nothing there
([explored-designs.md, "zram: prefetch the compressed data", its first table](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
On the Mi 9T, adding the prefetch of the compressed data to the phone's module made warm reads on the
little A55 core 2.3 µs faster, and prefetching the destination as well cost the big A76 core 2 µs on
cold reads
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#7-the-phones-module-did-not-prefetch-the-compressed-data-23-µs-faster-warm-reads-on-the-a55)).
The conclusion recorded there is that "a backend should prefetch the compressed data only".

The code does not say why the first loop starts at `src + 64`. The likely reason: one of the decoder's
first loads is the u16 at the start of the page, which is in the first line, so a prefetch of that
line would be issued at nearly the same moment as the load that needs it. The VM's experimental
backend copies the same start, "every cache line from the second on".

Lines 80 and 81 check the output room and decode. `*dst_len < SEQLZ_PAGE` comes first, and `||`
stops there if it is true, so the decoder never writes into a buffer that is too small. Then
`seqlz_decode()` ([chapter 14](#ch-decode)) with `NULL` for the scratch. A page with coded literals
is invalid without a scratch, and `seqlz-fast` never writes one, so for this codec that rule only
matters for damaged input, which it rejects. Line 83 reports the decompressed length, always a whole
page.

@listing src/zram_seqlz.c 87-95

The codec itself is a constant struct with designated initializers. Fields not named are zero, so
`create_dctx` and `destroy_dctx` are null pointers, which is how the harness, like zram after the
split, knows that this codec has no decompression context. The object is not `static`: the harness's
list of codecs in `bench/bench_main.cpp` names `quetschn_codec_seqlz_fast` through the `extern`
declaration in `zram_codec.h`.

## seqlz-fast-lit: a second context for the scratch {#ch-zram-glue--fast-lit}

@listing src/zram_seqlz.c 97-106

Coded literals are decoded into a buffer first, 8 streams side by side, and the sequence loop then
reads them from there as if they were raw ([chapter 13](#ch-literals-dec)). That buffer is the
scratch, `SEQLZ_SCRATCH` bytes: a page plus 16, 4112 bytes for 4 KiB pages. The 16 bytes are there
because the decoder copies literals 16 bytes at a time and may read up to 15 bytes past the last one,
and because the last, partial round of `decode_literals()` writes up to 8 decoded bytes past the end
of the literals that no one reads.

So `seqlz-fast-lit` has two contexts per CPU: 8192 bytes for compression and 4112 for decompression,
12 304 bytes together. That is exactly what it had with one context before the split, so the split
costs seqlz no memory in the harness.

@listing src/zram_seqlz.c 108-122

`fast_lit_compress()` is `fast_compress()` with `1` for `coded` in line 117. Whether the page then has
coded literals is `code_literals()`'s decision, per page. `seqlz_compress()` writes the page with raw
literals first and then, if coding saves enough, codes them over the raw ones in the same buffer, so
the same 2 pages of output suffice. `s->context` is the compression context here too, the same
8192-byte `struct seqlz_state` that `code_literals()` borrows to hold the raw literals.

@listing src/zram_seqlz.c 124-135

`fast_lit_decompress()` differs from `fast_decompress()` in one argument: line 131 passes
`s->context` as the scratch, and here `s` is the decompression context from `create_dctx()`. So
there is no `(void)s;`. The same function decodes pages with raw literals too; they leave the scratch
alone.

@listing src/zram_seqlz.c 137-147

The second codec fills in all eight function pointers. Both share `setup()`, `release()`,
`create_cctx()` and `destroy()`; only `create_dctx` and the two page functions are its own.

::: warn The harness prefetches the destination, the measurements say not to
`quetschn_prefetch_page()` prefetches the 4096 bytes of the destination as well as the compressed
data, and it does so for every codec that calls it, `lz4-prefetch` and both seqlz codecs. The kernel
measurements found the destination prefetch useless on x86-64 and harmful on the A76. Harness numbers
for seqlz's decode time therefore include a prefetch that the kernel's backend does not do. The
kernel's numbers, from the VM and the phone, are the ones the project trusts for decode times.
:::

## The kernel's copy is generated, not written {#ch-zram-glue--port}

`src/` is written to be kernel code, but it is not yet a patch for the kernel. A patch for
`lib/` needs the kernel's headers instead of `seqlz_compat.h`, `static` on every function only one
file calls, the exports, a `Kconfig` entry, a `Makefile`, the zram backend, documentation, tests in
the kernel's framework, and none of the harness's extra functions. Keeping a second copy of the codec
by hand would let the two drift apart. So there is one source, and a script writes the kernel's copy
from it: `tools/kernel-port/port.py <linux tree>`. Its docstring opens with the rule: "src/ stays the
one source of the codec."

What it writes into a Linux tree:

| file in the tree | from |
| --- | --- |
| `include/linux/seqlz.h` | `tools/kernel-port/seqlz.h`: the interface, two functions |
| `lib/seqlz/seqlz.h`, `seqlz_internal.h` | `src/`, without what only the harness uses |
| `lib/seqlz/seqlz_codes.c`, `seqlz_compress.c`, `seqlz_decompress.c` | `src/`, each with its part of the interface from `api_*.c`; the matcher of `page_lz.h` inside `seqlz_compress.c` |
| `lib/seqlz/seqlz_tables.c` | generated in the tree by `scripts/gen-seqlz-tables.py` from `lib/seqlz/seqlz_counts.txt` |
| `lib/seqlz/tests/seqlz_kunit.c` | `tools/kernel-port/seqlz_kunit.c`, with values from `docs/format.md` filled in |
| `lib/Kconfig`, `lib/Makefile`, `lib/seqlz/Makefile` | `CONFIG_SEQLZ` and `CONFIG_SEQLZ_KUNIT_TEST`, `-O3` as `lib/lz4` |
| `drivers/block/zram/backend_seqlz.[ch]`, `Kconfig`, `Makefile`, `zcomp.c` | the backend, `CONFIG_ZRAM_BACKEND_SEQLZ` |
| `Documentation/staging/seqlz.rst` | `docs/format.md`, by `format_rst.py` |
| `MAINTAINERS`, `Documentation/admin-guide/blockdev/zram.rst` | an entry each |
| `arch/x86/include/asm/processor.h` | `x86-prefetcht0.patch`, unless the tree has it |

The kernel copy is generated, never edited. A fix goes into `src/` or into `tools/kernel-port/`, and
`port.py` writes the copy again.

### What port.py changes in the code

Most of what `port.py` does is small and mechanical. Run on a tree at the commit CI uses,
`986c24e0fe44`, the diff between `src/seqlz_decompress.c` and the copy it writes is this, shortened:

```text
-// SPDX-License-Identifier: MIT OR GPL-2.0-only
+// SPDX-License-Identifier: GPL-2.0-only OR MIT
+#include <linux/cache.h>
+#include <linux/errno.h>
+#include <linux/export.h>
+#include <linux/prefetch.h>
+#include <linux/seqlz.h>
+#include <linux/unaligned.h>
+
-	if (br->count < (int)(SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS))
+	if (br->count < (int)(SEQLZ_TOKEN_BITS + PAGE_SHIFT))
-int seqlz_decode(const struct seqlz_tables *t, const void *src,
-		 unsigned int src_len, void *dst, void *scratch)
+/**
+ * seqlz_decode() - Decompress one page
+ ...
+ */
+static int seqlz_decode(const struct seqlz_tables *t, const void *src,
+			unsigned int src_len, void *dst, void *scratch)
+/* ---- the kernel's interface, include/linux/seqlz.h ---- */
+
+int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
+		     void *wrkmem)
+{
+	return seqlz_decode(&seqlz_fixed_tables, src, src_len, dst, wrkmem);
+}
+EXPORT_SYMBOL_GPL(seqlz_decompress);
```

Each change, and why:

- **The SPDX line in the kernel's order.** The licence stays the same; the kernel writes the
  GPL first.
- **The kernel's headers.** Each file gets the `#include <linux/...>` lines it needs, from a list in
  `port.py`. In the kernel, `seqlz_compat.h` would pull them in anyway (its `#ifdef __KERNEL__` half,
  [chapter 5](#ch-compat)), but a reviewer expects to see a file's includes in the file.
- **`PAGE_SHIFT` instead of `QUETSCHN_PAGE_BITS`,** in code and in comments. The block that defaults
  `QUETSCHN_PAGE_BITS` to 12 is cut. `lib/seqlz/seqlz.h` gets `#if PAGE_SHIFT != 12` and an
  `#error "seqlz is for 4 KiB pages only"`, and the values for 16 KiB pages are cut from the
  constants, e.g. `SEQLZ_RAW_NIBBLES` becomes the literal `0x95c840U`. The format for 16 KiB pages is
  not fixed yet, and with 16 KiB pages `seqlz-fast-lit` needs more work memory than the project
  allows itself ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).
- **`static` and kernel-doc.** `seqlz_decode()`, `seqlz_tables_init()` and `seqlz_compress()` of
  `src/` are called from one file only in the kernel, so they become `static`, and their kernel-doc
  comments move from the header to the definition. `make_static()` rewrites the signature and wraps
  the parameters again under the opening parenthesis, as the kernel's style wants.
- **`__init` on the table builders.** `seqlz_tables_init()`, `build_token()`, `build_values()`,
  `build_lit()`, `build_lit_sets()`, `first_codes()` and `length_entry()` run once at boot, so they
  are marked `__init`, and the kernel frees their code afterwards.
- **A new name for the compressor.** The interface takes the name `seqlz_compress()`, so the function
  of `src/` with that name becomes `seqlz_compress_page()`.
- **The matcher inside `seqlz_compress.c`.** `page_lz.h` is pasted right before `compress_page()`,
  the one function that calls it, and its `emit` callback becomes a direct call of `encode_emit()`. In
  `src/` the matcher takes a function pointer because it has two users, the compressor and
  `seqlz_find()`; in the kernel only one is left, and a call through a pointer would be a needless
  indirection for a reviewer to follow, even if the compiler inlines it.
- **What only the harness uses is cut.** `seqlz_find()`, `seqlz_encode()`, `struct seqlz_sequence`,
  `SEQLZ_MAX_SEQUENCES`, `seqlz_tables_size()`, and the `all_symbols` field with its check. The `raw`
  parameter of `code_literals()`, which only `seqlz_encode()` passes, goes too.

Every textual replacement goes through `must()` or `cut()`, which assert that the text they replace
occurs exactly once (or a given number of times). A change in `src/` that moves such a text makes
`port.py` stop with an assertion, instead of writing a copy that silently missed a step.

### What port.py refuses

After writing everything into memory, and before writing anything to disk, `port.py` checks every
line of every file it produced against three regular expressions:

```python
HARNESS_ONLY = re.compile(r"\bseqlz_find\b|\bseqlz_encode\b|\bseqlz_sequence\b|"
                          r"\bSEQLZ_MAX_SEQUENCES\b|\bseqlz_tables_size\b|\ball_symbols\b")
CPU_MODEL = re.compile(r"read_cpuid|\bMIDR_|boot_cpu_data|x86_model")
FORBIDDEN = re.compile(r"docs/|explored|quetschn|QUETSCHN|tools/|bench/|CMake|\.inc\b|src/|"
                       r"#\d{2,3}\b|seqlz-fast|zramphone|\bdump\b|\bcorpus\b|"
                       r"4\.14|Mi 9T|Fedora|kernel VM|\bthe phone|\bthe PC\b")
```

`HARNESS_ONLY` catches a harness function that is still named somewhere, e.g. in a comment.
`CPU_MODEL` applies to `lib/` only: nothing in the library may depend on which CPU model it runs on.
The decoder once had two loops, one for in-order cores chosen by the arm64 MIDR register, and the
review of the kernel copy ([#152](https://github.com/martinus/quetschn/issues/152)) replaced them with one loop for all cores; this check keeps it that
way. `FORBIDDEN` catches references a kernel reader cannot follow: the repository's documents and
tools, issue numbers like `#152`, the names `seqlz-fast` and `quetschn`, and reasons that only hold
for the project's machines, such as the Mi 9T's 4.14 kernel. The one URL allowed is the project's
page in `MAINTAINERS`.

Comments in `src/` do contain such things, on purpose: they quote the headings of
`docs/explored-designs.md` in double quotes, so that a reader of `src/` finds the measurement. In the
kernel those quotes are cut by a regular expression, and whole sentences are replaced through a list,
`REWRITES`, of pairs "old text, new text", e.g. `"docs/format.md is the specification. ..."` becomes
`"Documentation/staging/seqlz.rst specifies the format."`. A comment that changed is wrapped again to
80 columns in the kernel's block form. `port.py` counts how often each rewrite matched, and fails if
one matched nothing: a rewrite for a comment that no longer exists is stale and has to go with it.

## include/linux/seqlz.h: two functions {#ch-zram-glue--interface}

The kernel's interface to seqlz is not `src/seqlz.h`. It is a separate header,
`tools/kernel-port/seqlz.h`, which `port.py` copies to `include/linux/seqlz.h` as it is:

```c
/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * seqlz: compression of single memory pages of 4 KiB, for zram. An LZ format
 * whose sequences and literals are Huffman coded with tables that are part of
 * the format, so a page carries none. Documentation/staging/seqlz.rst specifies
 * the format.
 */
#ifndef _LINUX_SEQLZ_H
#define _LINUX_SEQLZ_H

#include <asm/page.h>

/* the work memory of seqlz_compress(): the matcher's table of positions */
#define SEQLZ_MEM_COMPRESS 8192
/* the work memory of seqlz_decompress(): the decoded literals of a page */
#define SEQLZ_MEM_DECOMPRESS (PAGE_SIZE + 16)

/* the literals as they are, or Huffman coded where that saves enough */
#define SEQLZ_LEVEL_RAW 1
#define SEQLZ_LEVEL_CODED 2

/**
 * seqlz_compress() - Compress one page
 * @src: the page, PAGE_SIZE bytes
 * @dst: where the compressed page goes
 * @dst_len: the bytes at @dst; every page fits into 2 * PAGE_SIZE
 * @wrkmem: SEQLZ_MEM_COMPRESS bytes, as kmalloc() aligns them
 * @level: SEQLZ_LEVEL_RAW or SEQLZ_LEVEL_CODED
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: the length of the compressed page, -E2BIG if it does not fit into
 * @dst_len bytes, or -EINVAL for another @level. The page is written with its
 * literals raw first, also at SEQLZ_LEVEL_CODED, and -E2BIG comes as soon as
 * that is within 32 bytes of @dst_len, even where the page with coded literals
 * would be smaller.
 */
int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level);

/**
 * seqlz_decompress() - Decompress one page
 * @src: the compressed page
 * @src_len: its length
 * @dst: PAGE_SIZE bytes for the page
 * @wrkmem: SEQLZ_MEM_DECOMPRESS bytes, or NULL if every page it gets was
 *          compressed at SEQLZ_LEVEL_RAW
 *
 * Safe for any input: it never reads outside @src and @src_len, and never
 * writes outside @dst and @wrkmem, and its time is bounded by the page size.
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: 0, or -EINVAL if @src is not a valid page, or a page with coded
 * literals and @wrkmem is NULL.
 */
int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
		     void *wrkmem);

#endif
```

This is the shape of `include/linux/lz4.h`: work memory sizes as constants, and functions that take
the work memory as a `void *`. The tables are not in it; they are the library's business. Before
commit `aaa381e` the kernel copy exported all of `src/seqlz.h`, 7 functions and the code lengths, 2
of them with no user in the kernel. Now there are two.

The comments are **kernel-doc**, the kernel's format for documenting functions: `/**` to open,
`name() - summary`, one `@parameter:` line each, then free text, and the sections `Context:` and
`Return:`. The kernel's `scripts/kernel-doc` parses them into the documentation and warns when a
parameter is missing; `check.sh` runs it on the codec's files in `lib/seqlz/` and on this header.

Some details of the text:

- `SEQLZ_MEM_COMPRESS` is 8192, the matcher's table. `SEQLZ_MEM_DECOMPRESS` is a page plus 16, the
  scratch of `src/seqlz.h`'s `SEQLZ_SCRATCH`. The library checks at build time that the two agree with
  `src/`'s constants, see below.
- `Context: Any context` is a promise to callers: the functions may also run with preemption
  disabled or in atomic context, where sleeping is not allowed. zram calls them under a mutex, which
  would allow sleeping, so zram does not need the promise; another caller might. It holds because the
  codec never calls anything that sleeps or allocates.
- The `Return:` of `seqlz_compress()` spells out a subtlety of [chapter 10](#ch-encoder): the encoder
  stops as soon as fewer than 32 bytes (`ENC_ROOM`) plus the next literals are free, so a page can be
  refused although its final size, with coded literals, would have fit. With zram's 2 pages it never
  happens.
- "Safe for any input" is constraint C6 of the plan, the one that stopped the last codec proposed for
  zram ([chapter 1](#ch-zram)).

### The two exported functions

`port.py` appends `tools/kernel-port/api_compress.c` to the compressor:

```c
/* ---- the kernel's interface, include/linux/seqlz.h ---- */

int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level)
{
	unsigned int len;

	if (level != SEQLZ_LEVEL_RAW && level != SEQLZ_LEVEL_CODED)
		return -EINVAL;
	len = seqlz_compress_page(&seqlz_fixed_tables, wrkmem, src, dst,
				  dst_len, level == SEQLZ_LEVEL_CODED);
	return len ? (int)len : -E2BIG;
}
EXPORT_SYMBOL_GPL(seqlz_compress);
```

It checks the level, calls the compressor of `src/` with the global tables, and maps its 0 to the
kernel's error code for "output too small". The cast `(int)len` is safe because a compressed page is
at most `dst_len` bytes, and `dst_len` is an `unsigned int` the caller sized; zram passes 8192.
`wrkmem` goes in as the `struct seqlz_state *` without a cast, which C allows from `void *`.

`api_decompress.c` is shorter still: `seqlz_decompress()` calls `seqlz_decode()` with
`&seqlz_fixed_tables` and returns its result, 0 or `-EINVAL`.

::: kernel EXPORT_SYMBOL_GPL
The kernel and its modules are linked at run time: when a module loads, its calls to functions
outside it are resolved against a table of exported symbols. A function is in that table only if its
file says so, with `EXPORT_SYMBOL(name)` or `EXPORT_SYMBOL_GPL(name)`. The `_GPL` variant makes it
available only to modules whose licence is GPL-compatible, as declared by `MODULE_LICENSE()`. New
kernel interfaces are usually exported this way. zram's backend for seqlz can be part of a zram module,
so it needs the export to call `seqlz_compress()`.
:::

## The tables, built once at boot {#ch-zram-glue--init}

`port.py` appends `tools/kernel-port/api_codes.c` to `seqlz_codes.c`:

```c
/* ---- the kernel's interface, include/linux/seqlz.h ---- */

/*
 * The codes and decode tables, built once from the code lengths compiled in,
 * and only read after that, by every CPU.
 */
struct seqlz_tables seqlz_fixed_tables __ro_after_init;

/* subsys_initcall: the tables are there before a zram that is built in */
static int __init seqlz_init(void)
{
	BUILD_BUG_ON(sizeof(struct seqlz_state) != SEQLZ_MEM_COMPRESS);
	BUILD_BUG_ON(SEQLZ_MEM_DECOMPRESS != SEQLZ_SCRATCH);
	/*
	 * Does not fail: before scripts/gen-seqlz-tables.py writes
	 * seqlz_tables.c, it checks the lengths as seqlz_tables_init() does,
	 * every table a complete prefix code with a code for every symbol, and
	 * the KUnit tests check that the lengths are the format's.
	 */
	return seqlz_tables_init(&seqlz_fixed_tables, &seqlz_default_own);
}
subsys_initcall(seqlz_init);

static void __exit seqlz_exit(void)
{
}
module_exit(seqlz_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("seqlz: LZ compression of memory pages with static Huffman codes");
```

`seqlz_fixed_tables` is the one `struct seqlz_tables` of the whole kernel, 43 280 bytes, shared by
every zram device and every CPU. It is global and not per CPU because it is only read after boot, and
read-only data can be shared by any number of CPUs without cost: each core keeps its own copy of the
cache lines it reads.

::: kernel __init, __ro_after_init and initcalls
**`__init`** puts a function into a separate section of the kernel image, `.init.text`. After boot,
the kernel frees that whole section, so code that runs only once at start costs no memory afterwards.
Calling an `__init` function later would jump into freed memory, so the build checks that only other
`__init` code calls it.

**`__ro_after_init`** puts a variable into a section that is writable during boot and becomes
read-only when boot is done (for a module, when its init function has returned). After that, a stray
write to the tables faults instead of corrupting every page compressed later. The section is
initialized data, so the 43 280 bytes are zeros in the kernel image until `seqlz_init()` fills them.

**Initcalls** are the kernel's way to run setup functions at boot, in a fixed order of levels:
`core`, `postcore`, `arch`, `subsys`, `fs`, `device`, `late`. A driver that is built in uses
`module_init()`, which is the `device` level. `subsys_initcall()` runs earlier, so seqlz's tables are
built before zram's own init can create a device. When the code is built as a module instead,
`subsys_initcall()` means the same as `module_init()`: the function runs when the module loads, and
the module loads before zram's because zram's backend needs its exported symbols.
:::

`BUILD_BUG_ON(condition)` fails the build if the condition is true, like a `static_assert` with the
kernel's older spelling. The two lines tie the interface's constants to the library's: the work
memory a caller allocates, `SEQLZ_MEM_COMPRESS`, must be exactly `sizeof(struct seqlz_state)`, and
`SEQLZ_MEM_DECOMPRESS` must be `SEQLZ_SCRATCH`. The interface header cannot include the library's
private header to get these values, so the check is where both are visible.

The comment explains why the return value of `seqlz_tables_init()` needs no special handling. The
code lengths in the kernel's copy are in `lib/seqlz/seqlz_tables.c`, which `port.py` has
`scripts/gen-seqlz-tables.py` generate in the tree from `lib/seqlz/seqlz_counts.txt`, the symbol
counts of the training. The generator checks the lengths before it writes them, with the same rules
as `seqlz_tables_init()`: no code too long, codes that fill each decode table exactly, a code for
every length value and literal, an escape code short enough. And the KUnit tests compare a SHA-256 of
the lengths with the one in the format's specification. So `seqlz_init()` cannot fail. Before commit
`4c42cc5`, of 10th October 2026, it had a `WARN_ON()` and could return `-EINVAL`: as a module seqlz
then did not load, and built in, zram would have compressed with half-built tables, because the
per-page check of `all_symbols` is not in the kernel copy.

`seqlz_exit()` is empty, and still needed: a module that has an init function and no exit function
cannot be unloaded. `__exit` lets a built-in kernel drop the function entirely.

`MODULE_LICENSE("Dual MIT/GPL")` is the kernel's spelling of the dual licence; the module loader
counts it as GPL-compatible, so the module may use GPL-only exports. `MODULE_DESCRIPTION` is the text
`modinfo` shows.

::: kernel A module, built in or loadable
Kernel code in `lib/` or `drivers/` can be compiled into the kernel image (`=y` in the configuration)
or into a separate file, a **module** (`=m`), that is loaded later with `modprobe`. The same source
works both ways: macros like `module_init()` and `MODULE_LICENSE()` expand to what each case needs.
`CONFIG_SEQLZ` is a tristate, so it can be either. In practice it is built in whenever zram's seqlz
backend is enabled, even when zram itself is a module: `CONFIG_ZRAM_BACKEND_SEQLZ` is a bool that
selects `SEQLZ`, as `ZRAM_BACKEND_LZ4` selects `LZ4_COMPRESS`.
:::

The generated `seqlz_tables.c` also has two lines the rest of the copy does not:
`EXPORT_SYMBOL_IF_KUNIT(seqlz_default_own);` and `EXPORT_SYMBOL_IF_KUNIT(seqlz_lit_sets);`. They
export the code lengths only in a kernel built with KUnit, into a separate symbol namespace that the
test module imports with `MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING")`. A normal kernel exports
nothing but the two functions.

## backend_seqlz.c: levels, and 8 KiB per CPU {#ch-zram-glue--backend}

The kernel's zram backend is `tools/kernel-port/backend_seqlz.c`, modelled on zram's
`backend_lz4.c`. It is a template: zram trees with and without the October 2026 series need different
code, and `port.py` keeps one of the two variants.

```c
#define pr_fmt(fmt) "seqlz: " fmt

#include <linux/kernel.h>
#include <linux/seqlz.h>
#include <linux/slab.h>

#include "backend_seqlz.h"

#ifndef ZCOMP_RW_SPLIT
/*
 * The work memory of compression and of decompression, per CPU. zram holds
 * the stream's mutex for either, and neither keeps anything in it from one
 * page to the next, so they share it: kmalloc() gives 8 KiB for the larger,
 * 16 KiB for both together.
 */
union seqlz_ctx {
	u8 cmem[SEQLZ_MEM_COMPRESS];
	u8 dmem[SEQLZ_MEM_DECOMPRESS];
};
#endif

static void seqlz_release_params(struct zcomp_params *params)
{
}

/*
 * Level 1 keeps the literals as they are, level 2, the default, Huffman codes
 * them where that saves enough: smaller pages for more time.
 * The tables are the library's, built once.
 */
static int seqlz_setup_params(struct zcomp_params *params)
{
	if (params->dict_sz) {
		pr_err("dictionary is not supported\n");
		return -EOPNOTSUPP;
	}
	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = SEQLZ_LEVEL_CODED;
	if (params->level != SEQLZ_LEVEL_RAW &&
	    params->level != SEQLZ_LEVEL_CODED) {
		pr_err("compression level %d is not supported\n",
		       params->level);
		return -EINVAL;
	}
	return 0;
}
```

`pr_fmt` must be defined before the first include: the kernel's `pr_err()` and friends expand it
around their format string, so every message of this file starts with `seqlz: `.

`ZCOMP_RW_SPLIT` is not a real kernel macro. It is a marker for `port.py`, which looks at the target
tree's `drivers/block/zram/zcomp.h`: if it has `struct zcomp_cstrm`, the tree has the series, and the
`#ifdef ZCOMP_RW_SPLIT` parts are kept, else the `#ifndef` parts. The lines `#ifdef`, `#else` and
`#endif` of the marker are removed, so the copy in the tree has no trace of it.

`seqlz_release_params()` is empty but must exist: zram calls `release_params()` through the pointer
without checking it for `NULL`, and seqlz has nothing per device to release, since `setup_params()`
allocates nothing.

`seqlz_setup_params()` is where the harness's `setup()` differs most. A dictionary is refused with
`-EOPNOTSUPP` and a message, where the harness ignores it. zram's "not set" level becomes level 2,
coded literals, the default. Any level other than 1 and 2 is `-EINVAL`, as `lz4` refuses invalid
levels. An administrator selects the level per device:

```text
echo "algo=seqlz level=1" > /sys/block/zram0/algorithm_params
```

Then the contexts, in the form without the split:

```c
static int seqlz_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	union seqlz_ctx *c;

	BUILD_BUG_ON(sizeof(*c) > KMALLOC_MAX_CACHE_SIZE);
	c = kvzalloc_obj(*c);
	if (!c)
		return -ENOMEM;
	ctx->context = c;
	return 0;
}

static void seqlz_destroy(struct zcomp_ctx *ctx)
{
	kvfree(ctx->context);
}
```

::: kernel kmalloc(), slabs, and what an allocation really costs
`kmalloc(size)` does not hand out `size` bytes. It takes an object from one of a fixed set of
**slab caches**, each with objects of one size: `kmalloc-8`, `kmalloc-16`, up to powers of 2, plus a
few sizes in between such as 96 and 192. A request is rounded up to the next cache. The largest cache
is `KMALLOC_MAX_CACHE_SIZE`, 2 pages, 8192 bytes on a kernel with 4 KiB pages. A larger request
bypasses the caches and takes whole pages from the page allocator, rounded up to a power of 2 pages.

`vmalloc(size)` maps single pages into a contiguous virtual range, rounded up to whole pages. It
works for any size but is slower and uses page table entries.

`kvzalloc(size, flags)` tries `kmalloc()` first and falls back to `vmalloc()`, and zeroes the memory
(the `z`). `kvzalloc_obj(*c)` is a newer typed spelling of `kvzalloc(sizeof(*c), GFP_KERNEL)`.
`GFP_KERNEL` means the allocation may sleep to free memory, which is fine here: contexts are created
when a CPU comes up or a device is set up, not in the path of a page.
:::

So the bytes that count per CPU are what the allocator hands out. Without the union, a struct of the
hash table and the scratch side by side was $8192 + 4112 = 12\,304$ bytes, above `KMALLOC_MAX_CACHE_SIZE`,
and `kvzalloc()` took 16 KiB for it. As a union it is 8192 bytes, the larger member, and comes from
`kmalloc-8k`: 8 KiB per CPU. For comparison, `lz4` asks `vmalloc()` for 16 416 bytes, which is 5 pages,
20 480 bytes
([plan.md §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)).
Against `lz4`'s 20 KiB, that is 12 KiB less per CPU; on the Mi 9T with its 8 cores, 96 KiB less memory,
resident for good whether the phone swaps or not.

::: why The union is safe because of zram's mutex
Compression and decompression share the bytes. That is safe for two reasons, both from the commit that
made the change, `6063362` ([#154](https://github.com/martinus/quetschn/issues/154)). zram holds the stream's mutex for either operation, so they never
run at the same time on one context. And neither keeps anything in the memory from one page to the
next: the matcher clears its table at the start of every page, and the decoder writes its scratch
before it reads it. The `BUILD_BUG_ON` keeps the union within `KMALLOC_MAX_CACHE_SIZE`: whoever turns
it back into a struct gets a build error, not a silent 16 KiB. The commit also ran the kernel copy
under KASAN, lockdep and UBSan: the corpus written and read back at both levels, 0 of 20 000 pages
different; 10 minutes of swap with 4 workers per boot, 16.5 million pages out at level 2 and 16.9
million at level 1, 0 different.
:::

With the split, the backend has two functions, and the union is not compiled:

```c
static int seqlz_create_cctx(struct zcomp_params *params,
			     struct zcomp_ctx *ctx)
{
	ctx->context = kvzalloc(SEQLZ_MEM_COMPRESS, GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

/* level 1 decompresses without work memory, its pages have no coded literals */
static int seqlz_create_dctx(struct zcomp_params *params,
			     struct zcomp_ctx *ctx)
{
	if (params->level == SEQLZ_LEVEL_RAW)
		return 0;
	ctx->context = kvzalloc(SEQLZ_MEM_DECOMPRESS, GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}
```

Here the two contexts are separate allocations, 8192 and 4112 bytes, both from `kmalloc-8k`: 16 KiB
per CPU at level 2, 8 KiB at level 1, which has no decompression context at all. `seqlz_destroy()`
serves both, and `kvfree(NULL)` does nothing, so a level-1 decompression context needs no special
case. Which of the two contexts the plan's memory limit, C5, applies to after the split is an open
question of the plan.

The page functions:

```c
static int seqlz_zcomp_compress(struct zcomp_params *params,
				struct zcomp_ctx *ctx, struct zcomp_req *req)
{
#ifdef ZCOMP_RW_SPLIT
	void *wrkmem = ctx->context;
#else
	void *wrkmem = ((union seqlz_ctx *)ctx->context)->cmem;
#endif
	int ret;

	if (req->src_len != PAGE_SIZE)
		return -EINVAL;
	ret = seqlz_compress(req->src, req->dst, req->dst_len, wrkmem,
			     params->level);
	if (ret < 0)
		return ret;
	req->dst_len = ret;
	return 0;
}

static int seqlz_zcomp_decompress(struct zcomp_params *params,
				  struct zcomp_ctx *ctx, struct zcomp_req *req)
{
#ifdef ZCOMP_RW_SPLIT
	void *wrkmem = ctx->context;
#else
	void *wrkmem = ((union seqlz_ctx *)ctx->context)->dmem;
#endif
	int ret;

	if (req->dst_len < PAGE_SIZE)
		return -EINVAL;
	ret = seqlz_decompress(req->src, req->src_len, req->dst, wrkmem);
	if (ret)
		return ret;
	req->dst_len = PAGE_SIZE;
	return 0;
}
```

They are the harness's functions in the kernel's types. zram passes a `struct zcomp_req` with
`src`, `src_len`, `dst` and `dst_len`; `zcomp_compress()` sets `dst_len` to `2 * PAGE_SIZE`, and
`zcomp_decompress()` to `PAGE_SIZE`. The level comes from the params instead of from the codec's name.
In the union form, `->cmem` and `->dmem` are the same address; naming the member says which use is
meant. Neither function prefetches: the prefetch of the compressed data that the measurements above
recommend is not part of the kernel's backend; the VM's measurements of it used experimental code,
`tools/zram-vm/zram-prefetch.patch` and the VM's own backend in `tools/zram-vm/backend_seqlz.c`.

The struct at the end, `const struct zcomp_ops backend_seqlz`, names these functions, with
`create_cctx`, `destroy_cctx`, `create_dctx`, `destroy_dctx` or `create_ctx`, `destroy_ctx` depending on
the tree, and `.name = "seqlz"`. `port.py` adds `&backend_seqlz` to the list of backends in
`zcomp.c`, the entries to zram's `Kconfig` and `Makefile`, and a paragraph to zram's documentation.

## prefetch() on x86-64: a patch to the kernel itself {#ch-zram-glue--prefetch-patch}

The decoder prefetches its tables with the kernel's `prefetch()` before it decodes a page
([chapter 12](#ch-dec-basics)). On x86-64 that was `__builtin_prefetch()`, which clang drops under the
kernel's `-mno-sse`, so a kernel built with clang, as Android's kernels are, had no prefetches in the
decoder; [chapter 5](#ch-compat--prefetch) tells the story and gives the patch's measurement. No inline
assembly goes into `lib/`, so the fix is a patch to the kernel, `tools/kernel-port/x86-prefetcht0.patch`,
a commit for `arch/x86/include/asm/processor.h` that defines `ARCH_HAS_PREFETCH` for 64-bit and
implements `prefetch()` as one `prefetcht0` instruction:

```c
#ifdef CONFIG_X86_64
/*
 * Every x86-64 CPU has prefetcht0. Not __builtin_prefetch(): the kernel
 * builds with -mno-sse, and clang then drops the builtin without a warning.
 */
static inline void prefetch(const void *x)
{
	asm("prefetcht0 %0" : : "m" (*(const char *)x));
}
#else
```

In the measurement of the patch's commit message, `lz4`, which does not prefetch, swapped in the same
with and without it. The patch also says why it does not use the
alternative that 32-bit x86 has: that one patches in `prefetchnta`, a non-temporal prefetch, on every
CPU with SSE, which would change what gcc builds get. `port.py` applies the patch, unless the tree has
it already, and `src/seqlz_compat.h` does the same outside the kernel ([chapter 5](#ch-compat)).

## Checks a submission needs {#ch-zram-glue--check}

`tools/kernel-port/check.sh <linux tree> [make args]` is what CI runs on the kernel copy, once per
architecture. It takes the tree's `HEAD` with `git archive` into a temporary directory, so the tree
itself is never touched, applies the x86 patch to that base and commits it, so that checkpatch later
sees seqlz's part alone, runs `port.py`, and then:

1. Configures `defconfig` with zram and the seqlz backend built in, seqlz and its KUnit tests as
   modules, and fails if the configuration did not take them.
2. Builds `lib/seqlz/` and `drivers/block/zram/` with `W=1`, and fails on any line with `warning` or
   `error`.
3. On x86-64, counts the `prefetcht0` instructions in `seqlz_decompress.o` with `objdump`, and fails
   if there are none. With clang that is the check that the prefetch patch works.
4. Runs `checkpatch.pl --strict` on the whole diff, and on the x86 patch on its own, and prints the
   findings.
5. Runs `scripts/kernel-doc -none -Wall` on the codec's files and both headers.
6. Parses `Documentation/staging/seqlz.rst` with docutils, if Python has it.

::: kernel W=1, checkpatch and kernel-doc
**`W=1`** is a `make` option that turns on warnings the kernel leaves off by default because older
code triggers them, e.g. `-Wunused-const-variable`, `-Wmissing-format-attribute` and `-Wundef`, and it
keeps warnings that the normal build silences, e.g. `-Wunused-but-set-variable`. New code is expected
to build without warnings at `W=1`.

**`scripts/checkpatch.pl`** reads a patch and reports style problems: lines over the limit, missing
SPDX lines, `MAINTAINERS` not updated for a new file, spaces where tabs belong, and many more.
`--strict` adds the checks that maintainers of some subsystems ask for. Reviewers run it before they
read a patch.

**`scripts/kernel-doc`** reads the comments of [the interface](#ch-zram-glue--interface), and with
`-Wall` warns about undocumented parameters and malformed sections.
:::

CI's `kernel-port` job runs `check.sh` four times: x86-64 with gcc, and with clang (`LLVM=1`) for
arm64, 32-bit arm and s390. Each of these catches something the others do not. arm64 is the
architecture of phones. 32-bit arm has 32-bit `long` and pointers, so a computation that assumes 64
bits shows up there, and its kernel warns about stack frames above 1280 bytes (`CONFIG_FRAME_WARN`'s
default for 32-bit in the tree CI uses), against 2048 on 64-bit; since `check.sh` fails on any
warning, a frame that grows past that fails CI. s390 is big-endian. For a sense of the margin: in the kernel copy built as `check.sh` builds it for x86-64, `defconfig`
with gcc 16.2.1 and the Makefile's `-O3`, gcc's `-fstack-usage` gives 640 bytes each for
`code_literals()` and `decode_literals()`, the deepest frames. With KASAN and UBSan added, in their
default configuration, they grow to 1000 and 952 bytes. Since 11th October `check.sh` prints the three
deepest frames of each build and fails at 1024 bytes, the limit the project keeps. On the tree at
`986c24e0fe44` they were 560 bytes for `seqlz_compress()` on arm64, 384 on 32-bit arm and 536 on s390,
all with clang; the CI machine's compilers give a little different numbers, e.g. 648 for
`code_literals()` with Ubuntu's gcc.

### KUnit tests in UML

`port.py` also writes `lib/seqlz/tests/seqlz_kunit.c` from `tools/kernel-port/seqlz_kunit.c`, the
codec's tests in the kernel's own test framework. Six cases:

- `seqlz_test_tables`: the SHA-256 of each table's code lengths against the one `docs/format.md`
  gives. The template has placeholders like `"@TOK_4k@"`, which `port.py` fills from the
  specification's table, so the kernel's tests check against the specification, not against a copy.
- `seqlz_test_example`: the specification's worked example, 8 bytes that decode to a page of `ab`
  2048 times. The same bytes without the last one, and with one zero byte more, must both be
  `-EINVAL`: a page ends where its bits end.
- `seqlz_test_round_trip`: generated pages of 7 kinds, compressed and decompressed at both levels.
- `seqlz_test_damaged`: damaged pages are rejected or decode, but never read or write out of bounds.
- `seqlz_test_one_sequence`: pages built by hand from the specification's rules, each valid or
  invalid by one rule.
- `seqlz_test_coded_literals`: pages whose literals get coded.

The helper `decompress()` in that file copies the input into an allocation of exactly its length
before it decodes, "so that KASAN sees a read behind them": KASAN, the kernel's address sanitizer,
reports a read one byte past an allocation, but only if the allocation ends there.

::: kernel KUnit and UML
**KUnit** is the kernel's framework for unit tests. A test file defines functions that take a
`struct kunit *test`, checks results with macros like `KUNIT_EXPECT_EQ(test, a, b)` and
`KUNIT_ASSERT_NOT_NULL()`, and lists the functions as `KUNIT_CASE()`s of a suite. The tests run inside
a kernel, with the kernel's allocator and headers, so they test the code as the kernel builds it.

**UML**, User Mode Linux, is the Linux kernel built as an ordinary Linux program: `ARCH=um`. It boots
in a second, needs no virtual machine, and runs as a process on the host. `tools/testing/kunit/kunit.py
run` configures, builds and boots such a kernel with the tests built in, and reports their results.
:::

`tools/kernel-port/kunit.sh <linux tree>` does that for seqlz: `port.py` into a copy of the tree, then
`kunit.py run --kunitconfig=lib/seqlz`, which reads the two lines `port.py` wrote into
`lib/seqlz/.kunitconfig`, `CONFIG_KUNIT=y` and `CONFIG_SEQLZ_KUNIT_TEST=y`. CI runs it on x86-64 with
KASAN and UBSan added.

A third script, `tools/kernel-port/stress.sh`, goes further and is run by hand: it boots the kernel
copy in a VM with KASAN, lockdep, `DEBUG_ATOMIC_SLEEP` and UBSan, writes a corpus to zram devices with
`lz4` and seqlz at both levels, swaps under memory pressure with every page compared, and runs the
kernel's zram selftests. That is the gate of the plan's Phase 5, and the source of the page counts in
the `why` box above.

### The specification goes along

The format's specification, `docs/format.md`, also becomes kernel documentation:
`Documentation/staging/seqlz.rst`, written by `tools/kernel-port/format_rst.py`. The converter handles
only what `format.md` uses: three levels of headings, paragraphs, lists, pipe tables, fenced code,
inline code, bold, italics and links to its own headings or to the web. Anything else makes it fail,
"so that a new construct in format.md is noticed instead of turned into wrong reST". Two HTML comments,
which GitHub does not show, mark text that is only for the repository (`<!-- not in the kernel's
copy -->` up to `<!-- end -->`) and text that is only for the kernel (`<!-- in the kernel's copy:` up
to `-->`). The section on how the file was checked against the project's reference decoders is
repository-only.

## Testing outside the kernel {#ch-zram-glue--tests}

Most testing happens in userspace, where it is fast and the sanitizers are easy to use.

### Unit tests

`test/` holds the unit tests, written with doctest, a single-header C++ test framework. They all go
into one binary, `quetschn_test`; at commit `a2d57dd` it has 130 test cases, 27 of them in
`test/seqlz_test.cpp` for the codec, 18 in `test/kernel_codecs_test.cpp` for the harness's codecs
including this chapter's glue, and 8 in `test/seqlz_ref_test.cpp` for the reference decoder. The tests
are C++, the codec is C: `seqlz.h` is written so that C++ can include it (`seqlz_compat.h` does not
define `bool` itself, see [chapter 5](#ch-compat)).

Two tests check this chapter's file directly. `seqlz takes no dictionary, its tables are part of the
format` creates both codecs with a dictionary of exactly the size of `struct seqlz_lengths`, which
once replaced the code lengths, and checks that a page compresses to the same bytes as without one.
`seqlz compresses with the hash table, seqlz-fast-lit decompresses with the scratch` checks the
context sizes: `sizeof(seqlz_state)` for compression in both codecs, 0 bytes and no `create_dctx` for
`seqlz-fast`, `SEQLZ_SCRATCH` for `seqlz-fast-lit`, and that `seqlz-fast` decodes with a stream that
has no context at all.

### Fuzzing

A fuzzer feeds a function millions of generated inputs per minute, mutating the ones that reach new
code, and reports a crash, a sanitizer finding or a hang. `fuzz/` has four targets for libFuzzer (and
AFL++, which can run the same entry point):

- `seqlz_decode_fuzz.c`: any input through `seqlz_decode()`, with and without the scratch. Input, page
  and scratch each get an allocation of exactly their size, so that AddressSanitizer sees a read or
  write one byte too far. A page that decodes without the scratch must decode the same with it.
- `seqlz_roundtrip_fuzz.c`: any page through `seqlz_compress()` and `seqlz_decode()`, raw and coded.
  The page must come back, fit into 2 pages, and be the same bytes as `seqlz_encode()` writes for
  `seqlz_find()`'s sequences.
- `seqlz_encode_fuzz.c`: any list of sequences through `seqlz_encode()`, also lists that do not make a
  page. It was added after an encoder overflow ([#121](https://github.com/martinus/quetschn/issues/121)) that none of the other targets reached, because
  they only give the encoder the matcher's sequences, which are always valid.
- `seqlz_diff_fuzz.c`: the decoder against a second decoder.

The second decoder is `tools/seqlz-ref/seqlz_ref.c`. It is written from `docs/format.md` alone,
without looking at `src/`, "so that a misreading in src/ is not shared". It is slow on purpose: it
reads every bit on its own, finds every code by its canonical number, and checks every rule of the
format where the specification states it. The diff target gives each input to both decoders and
calls `abort()` if they disagree on whether the page is valid, or on the page it decodes to. A
disagreement is a bug in one of them, or a place where the specification is not clear. A Python
version, `tools/seqlz-ref/seqlz_ref.py`, does the same for the documentation checks.

`fuzz/smoke.sh` runs all four targets at once for 60 seconds each under AddressSanitizer and
UndefinedBehaviorSanitizer; CI runs it for 4 KiB pages, 16 KiB pages, and once with MemorySanitizer,
which reports a read of a byte the codec did not write for this input (`fuzz/unwritten.h` marks the
buffers as unwritten before each input). `fuzz/afl.sh` starts long AFL++ runs. On the diff target,
`docs/format.md` records two: on 6th October 2026, 10 AFL++ instances for 2 hours, 1.7 billion inputs,
no difference found; on 9th October, with the encoder and decoder of commit `5342a9b`, 12 instances
for 2 hours, 1.94 billion inputs, none either
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#how-this-file-was-checked)).
Outside CI, ClusterFuzzLite, a GitHub workflow that fuzzes on a schedule, runs the targets daily, an
hour each with AddressSanitizer, UndefinedBehaviorSanitizer and MemorySanitizer.

### The same bytes on a big-endian CPU

The format is little endian: the u16 at the start of a page is stored low byte first, and so are the
stream sizes. The code reads and writes such values with `get_unaligned_le16()` and its relatives,
never by casting a pointer. Until commit `91fa53e` on 5th October 2026 they did not, and on the big-endian
s390x every page failed to round trip ([chapter 5](#ch-compat--endian)).

`test/seqlz_endian.c` is the check that this stays fixed. It generates 600 pages of six kinds
(random bytes, a short pattern repeated, 8-byte numbers counting up, runs of a few byte values, words
from a small vocabulary, half zeros), compresses each with raw and with coded literals, decodes it
again, and hashes all compressed bytes with FNV-1a. It is plain C without the test framework, so
that it builds in seconds under qemu. Its output on x86-64, and the same with `-m32`:

```text
pages 600, coded 200, failed 0, hash fda8f1567c2bb3a1
```

`coded 200` counts the compressions whose page came out with coded literals, out of 1200. CI's
`same-bytes` job builds it three times, for x86-64, 32-bit x86 and s390x in a qemu container, and
`diff`s the three output lines. If any CPU writes one byte differently, the hashes differ.

### CI

`.github/workflows/ci.yml` has these jobs:

| job | what it runs |
| --- | --- |
| `format` | `clang-format` 21.1.8 from pip, on every `.c`, `.h` and `.cpp` |
| `test` | `quetschn_test` built 4 ways: gcc debug with sanitizers, clang release, gcc release on arm64, gcc release with sanitizers for 16 KiB pages |
| `fuzz` | `fuzz/smoke.sh 60` for 4 KiB, 16 KiB and MSan |
| `same-bytes` | `test/seqlz_endian.c` on x86-64, 32-bit x86 and s390x, outputs diffed |
| `docs` | `tools/seqlz-ref/check.sh` (table hashes in `docs/format.md`, pages decoded by the Python reference decoder) and `tools/check-links.py` |
| `kernel-port` | `check.sh` for x86-64 (gcc), arm64, arm and s390 (clang), and `kunit.sh` with KASAN and UBSan on x86-64 |
| `ci-ok` | the one required check |

The `test` job does not trust the exit code alone: it greps doctest's summary line for a non-zero
number of passed assertions and 0 failed, because a binary that ran no test at all also exits 0.

`ci-ok` exists because of how GitHub treats required checks. Branch protection requires only this
job. It `needs` all the others and runs with `if: always()`, then tests that each of them reported
`success`. Without `always()`, a failed dependency would make `ci-ok` skipped, and GitHub counts a
skipped required check as passed. With one required job, the others can be renamed or split without
touching the branch settings; a new job only has to be added to `ci-ok`'s `needs` and its list of
tests.

::: warn What none of this covers
All of these checks run the codec on CPUs and compilers that CI has. The decode times and the memory
numbers come from separate measurements, in the kernel VM and on the Mi 9T, which CI does not repeat.
And the harness glue of this chapter is not what the kernel runs: the kernel runs
`backend_seqlz.c`, which the KUnit tests and `stress.sh` exercise, but which the harness's benchmarks
reach only with `PORT=1` in the VM.
:::
