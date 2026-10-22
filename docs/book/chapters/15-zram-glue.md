# From a library to a zram backend {#ch-zram-glue}

Everything up to here is a library: functions that take a block and work memory, and know nothing
about their caller. `src/zram_seqlz.c`, {{ch15.zram_lines}} lines, is the piece between seqlz and zram
for the project's benchmark harness: it builds the tables, hands out the work memory per CPU,
checks the lengths and turns seqlz's return values into zram's. The kernel does not get this file. Its
copy is written by `tools/kernel-port/port.py` from the rest of `src/` and a few files of its own. This
chapter goes through `src/zram_seqlz.c`, then what changes on the way into the kernel, and ends with the
tests that hold it together.

## The harness's picture of zram {#ch-zram-glue--harness}

zram's interface to a backend, `struct zcomp_ops`, and its streams, one per CPU and locked by a mutex,
are in [chapter 1](#ch-zram--zcomp). The harness has a copy of that interface in
`bench/kernel_codecs/zram_codec.h`, shortened:

```c
struct quetschn_params {  /* struct zcomp_params: one per device */
	const void *dict;
	size_t dict_size;
	int level;
	unsigned page_size;   /* the harness runs 4 KiB and 16 KiB pages */
	void *drv_data;
	size_t allocated;     /* what the codec allocated, as the allocator hands it out */
};

struct quetschn_stream {  /* struct zcomp_ctx: one per CPU and direction */
	void *context;
	size_t allocated;
};

struct quetschn_codec {   /* struct zcomp_ops */
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

The two context functions are those of Sergey Senozhatsky's series of October 2026, which splits a
stream into one for writing and one for reading
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zrams-contexts-split-into-compression-and-decompression-the-same-time-in-the-vm-the-same-memory-for-seqlz)).
A codec without `create_dctx` gets the compression context in `decompress()`, as every backend did before
the split; the harness's `lz4`, `lzo` and `zstd` are copies of the kernel's glue from before it.

## Three codecs, one format, no dictionary {#ch-zram-glue--head}

@listing src/zram_seqlz.c 1-8

The three codecs are the three levels of `seqlz_compress()` ([chapter 6](#ch-header--compressor)); they
write the same format and are read by the same decoder. `seqlz-2` lets `seqlz_code_literals()`
([chapter 11](#ch-literals-enc)) code the literals where that saves $\frac{1}{16}$ of them and
{{ch15.coded_min}} bytes, `seqlz-3` adds the lazy matcher, a literal table of the page's own and the
literals as deltas to earlier bytes of the page. In the
kernel they are levels 1 to 3 of one backend, `seqlz`, with 2 the default; the harness names them as
three codecs to compare them side by side. A dictionary could have carried other
tables, e.g. tables trained on one phone's pages, and measured, those saved too little
([chapter 8](#ch-codes--fixed)). So since commit `87a0ba4` the tables are part of the format, and the
dictionary is ignored here; the kernel's backend refuses one. The file includes `seqlz.h`, the
userspace interface ([chapter 6](#ch-header--tables)).

## Setting up a device: the tables, built and checked {#ch-zram-glue--setup}

@listing src/zram_seqlz.c 10-25

`setup()` is `setup_params()`. seqlz is compiled for one block size, so a 4 KiB build given a corpus of
16 KiB pages fails here instead of reading past each page. Then it builds the one global
`seqlz_tables` from the code lengths ([chapter 8](#ch-codes)); a second device builds the same bytes
again. `seqlz_all_symbols()` checks that the encoder has a code for every symbol it may have to write;
with fixed tables that can only fail after a bad training, and then before the first page instead of
in the middle of a benchmark.

::: why One set of tables for all devices, here and in the kernel
The kernel's tables are `const` data, generated when the kernel is built ([below](#ch-zram-glue--init)),
and all devices and CPUs read them. Building {{ch15.tables_kb}} KB of tables in `setup_params()` for
every zram device and priority, from constant data, was what the kernel copy did until commit
`aaa381e`; the review asked for the global. Until 22nd October 2026 the harness still allocated a set
per device and passed it to every call, and every function of the codec took it as a parameter.
:::

@listing src/zram_seqlz.c 27-36

`destroy()` takes only the stream, so it serves as `destroy_cctx` and as `destroy_dctx`.

## The compression context is the matcher's table {#ch-zram-glue--cctx}

@listing src/zram_seqlz.c 38-47

`struct seqlz_state` is a union of the matcher's hash table and a page of bytes
([chapter 6](#ch-header--compressor)): {{ch17.hash_slots}} entries of 2 bytes, {{size.state}} bytes, and
`seqlz_code_literals()` copies the raw literals into the same memory once the matcher is done
([chapter 11](#ch-literals-enc--in-place)). Both codecs compress with it. The zeroing is not needed, since
the matcher clears its table for every page; `kzalloc()` zeroes in the kernel, and the harness does the
same. The kernel builds without the warning about unused parameters below `W=3`, so its backend needs no
`(void)p`.
@assert ch17.hash_slots * 2 == size.state and size.state >= 4096

## seqlz-1: compress and decompress {#ch-zram-glue--fast}

@listing src/zram_seqlz.c 49-62

`compress()` is the one compress function of all three codecs, with the level as a parameter that
`struct zcomp_ops` does not have; each codec gets a wrapper of one line that passes its level:

@listing src/zram_seqlz.c 64-69

`*dst_len` is the capacity of `dst` on the way in and the compressed length on the way out. zram gives a
compressor 2 pages of output, and a page always fits into 2 pages ([chapter 10](#ch-encoder)), so the 0
of a page that did not fit comes only with a smaller buffer, e.g. the one page zswap gives.

@listing src/zram_seqlz.c 71-86

`s` is the compression context here, because this codec has no `create_dctx`, and pages with raw
literals need no work memory. In zram with the split, a codec without `create_dctx` gets a stream with
no context at all; the test `seqlz compresses with the hash table, seqlz-2 decompresses with the
scratch` in `test/kernel_codecs_test.cpp` decodes every test page that way. Without a scratch, a page
with coded literals is invalid, which `seqlz-1` never writes.

The prefetch is in the harness's header:

@listing bench/kernel_codecs/zram_codec.h {{ch15.prefetch_page.a}}-{{ch15.prefetch_page.b}}

It asks for the compressed page from its second cache line on, and for every line of the destination
with the second argument `1`, "for writing". The CPU's own prefetcher follows a pointer walking forward,
but the decoder reads a page in several places at once: the literals, the bitstream behind them, and up
to 8 streams. The code does not say why it starts at `src + 64`; the first line is likely loaded by the
decoder's read of the header at about the same moment.

::: measured Prefetch the compressed data, not the destination
In the kernel VM, with the compressed data flushed from the cache, the prefetch of it took `lz4`'s p99
read latency from 5600 to 3891 ns, and seqlz's from 5450 to 4410 ns; the destination's prefetch helped
nothing there
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
On the Mi 9T, the prefetch of the compressed data made the A55's warm reads 2.3 µs faster, and the
destination's as well cost the A76 2 µs on cold reads
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#7-the-phones-module-did-not-prefetch-the-compressed-data-23-µs-faster-warm-reads-on-the-a55)).
The harness still prefetches the destination for every codec that calls it, `lz4-prefetch` and both seqlz
codecs, so its decode times include a prefetch the kernel's backend does not do. The kernel's numbers,
from the VM and the phone, are the ones the project trusts for decode times.
:::

@listing src/zram_seqlz.c 88-96

The fields not named are null, which is how the harness, like zram after the split, knows that this
codec has no decompression context. The object is not `static`: `bench/bench_main.cpp` names it.

## seqlz-2 and seqlz-3: a second context for the scratch {#ch-zram-glue--fast-lit}

@listing src/zram_seqlz.c 98-107

The scratch is where the coded literals are decoded to ([chapter 13](#ch-literals-dec)), a page plus 16
bytes, at levels 2 and 3, {{const.SCRATCH}} with 4 KiB pages: the decoder's 16-byte copies read up to 15 bytes past the last
literal, and the last step of `seqlz_decode_literals()` writes up to 8. So `seqlz-2` has
{{size.state}} bytes per CPU for compression and {{const.SCRATCH}} for decompression,
{{ch15.state_plus_scratch|grp}} bytes, which `kmalloc()` hands out as {{ch15.mem.split_gets|grp}}. The
harness's one context before the split was a struct of both, also {{ch15.mem.struct_gets|grp}} bytes
handed out, so the split cost seqlz no memory.

@listing src/zram_seqlz.c 109-121

`seqlz_compress()` writes the raw literals first and codes them over the raw ones in the same buffer
where that pays, so the same 2 pages of output suffice. Level 3 needs no more memory than level 2:
its table of the page's own is worked out in the compression context, behind the copy of the literals
([chapter 11](#ch-literals-enc--own)), and decoded into the output page and the scratch
([chapter 13](#ch-literals-dec--own)); the records for the deltas go into the output buffer, and the
deltas into the compression context ([chapter 10](#ch-encoder--records)).

@listing src/zram_seqlz.c 123-133

Here `s` is the decompression context, and pages with raw literals leave the scratch alone. One
`decompress()` serves `seqlz-2` and `seqlz-3`: the decoder reads from byte 2 which table a page uses,
and whether its literals are deltas.

@listing src/zram_seqlz.c 135-157

## The kernel's copy is generated, not written {#ch-zram-glue--port}

A patch for `lib/` needs the kernel's headers instead of `seqlz_compat.h`, `static` on every function
one file calls, exports, `Kconfig` and `Makefile` entries, the zram backend, documentation, KUnit tests,
and none of the harness's functions. A second copy by hand would drift, so `port.py <linux tree>`
writes it, and its docstring opens with the rule: "src/ stays the one source of the codec." A fix goes
into `src/` or `tools/kernel-port/`, never into the copy.

| file in the tree | from |
| --- | --- |
| `include/linux/seqlz.h` | `tools/kernel-port/seqlz.h`: the interface, two functions |
| `lib/seqlz/seqlz_format.h`, `seqlz_tables.h`, `seqlz_internal.h` | `src/`, without what only the harness uses |
| `lib/seqlz/seqlz_compress.c`, `seqlz_compress_literals.c`, `seqlz_decompress.c`, `seqlz_decompress_literals.c` | `src/`; `seqlz_compress.c` and `seqlz_decompress.c` each with its part of the interface from `api_*.c`, the matcher of `seqlz_match.h` inside `seqlz_compress.c` |
| `lib/seqlz/gen_seqlz_tables.c` | `src/seqlz_codes.c` and `gen_tables_main.c`: a program for the build machine, which writes the tables, `seqlz_tables.c`, when the kernel is built |
| `lib/seqlz/seqlz_lengths.h` | the code lengths, written in the tree by its `scripts/gen-seqlz-lengths.py` from `lib/seqlz/seqlz_counts.txt` |
| `scripts/gen-seqlz-lengths.py`, `lib/seqlz/seqlz_counts.txt` | `tools/kernel-port/gen-seqlz-lengths.py` and `bench/seqlz_counts_4k.txt` |
| `lib/seqlz/tests/seqlz_kunit.c` | `tools/kernel-port/seqlz_kunit.c`, with values from `docs/format.md` filled in |
| `lib/Kconfig`, `lib/Kconfig.debug`, `lib/Makefile`, `lib/seqlz/Makefile` | `CONFIG_SEQLZ`, and `CONFIG_SEQLZ_KUNIT_TEST` with the other KUnit tests of `lib/`; `-O3` as `lib/lz4` |
| `drivers/block/zram/backend_seqlz.[ch]`, `Kconfig`, `Makefile`, `zcomp.c` | the backend, `CONFIG_ZRAM_BACKEND_SEQLZ` |
| `Documentation/staging/seqlz.rst` | `docs/format.md`, by `format_rst.py` |
| `MAINTAINERS`, `Documentation/admin-guide/blockdev/zram.rst`, `Documentation/staging/index.rst` | an entry each |
| `arch/x86/include/asm/processor.h` | `x86-prefetcht0.patch`, unless the tree has it |

### What port.py changes in the code

The diff between `src/seqlz_decompress.c` and its copy, on the tree CI uses, `986c24e0fe44`, shortened:

```text
-// SPDX-License-Identifier: MIT OR GPL-2.0-only
+// SPDX-License-Identifier: GPL-2.0-only OR MIT
-#include "seqlz.h"
+#include <linux/errno.h>
+#include <linux/export.h>
+#include <linux/module.h>
+#include <linux/unaligned.h>
+
+#include "seqlz_internal.h"
- * number of extra bits, which follow the code. Takes up to SEQLZ_MAX_BITS +
- * SEQLZ_BLOCK_BITS bits, 20 or 22; the caller refills before.
+ * number of extra bits, which follow the code. Takes up to
+ * SEQLZ_MAX_BITS + SEQLZ_BLOCK_BITS bits, 20; the caller refills before.
-int seqlz_decode(const void *src, unsigned int src_len, void *dst,
-		 void *scratch)
+/* seqlz_decompress(), see include/linux/seqlz.h */
+static int seqlz_decode(const void *src, unsigned int src_len, void *dst,
+			void *scratch)
+/* the interface's work memory is the buffer of coded literals */
+static_assert(SEQLZ_MEM_DECOMPRESS == SEQLZ_SCRATCH);
+static_assert(SEQLZ_BLOCK_SIZE == 1U << SEQLZ_BLOCK_BITS);
+
+int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
+		     void *wrkmem)
+{
+	return seqlz_decode(src, src_len, dst, wrkmem);
+}
+EXPORT_SYMBOL_GPL(seqlz_decompress);
+
+MODULE_LICENSE("Dual MIT/GPL");
```

- **The kernel's headers**, from a list in `port.py`. `seqlz_compat.h` would pull them in anyway
  ([chapter 5](#ch-compat--kernel)), but a reviewer expects a file's includes in the file.
- **`#define SEQLZ_BLOCK_BITS 12U`** in place of `QUETSCHN_PAGE_BITS`, whatever the kernel's
  `PAGE_SIZE` is: the library compresses blocks of 4 KiB, `SEQLZ_BLOCK_SIZE` of the interface, and
  builds with 16 KiB and 64 KiB pages too. The values for 16 KiB blocks are cut, e.g.
  `SEQLZ_HASH_BITS` becomes `12U`, and so are the numbers for them in the comments, as in the diff
  above. The format for 16 KiB blocks is not fixed yet, and with them `seqlz-2` asks for twice `lz4`'s
  work memory per CPU ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).
  A library in `lib/` that built only with 4 KiB pages would fail the first build bot with 64 KiB pages
  on arm64.
- **`static`.** `seqlz_decode()` and `seqlz_compress()` of `src/` are called from one file in the
  kernel, so they become `static`, and their kernel-doc is the interface's, in `include/linux/seqlz.h`;
  a one-line comment points there.
- **`seqlz_compress_block()`.** The interface takes the name `seqlz_compress()`, and the codec's
  function is renamed, without its test of the level, which the interface does.
- **`seqlz_codes.c` as a program for the build machine**, `gen_seqlz_tables.c`
  ([chapter 8](#ch-codes--kernel)), without the global, `seqlz_tables_init()` and
  `seqlz_all_symbols()`.
- **The matcher inside `seqlz_compress.c`**, right before its callers, `compress_block()` and
  `compress_own()`, with its `emit` callback a direct call of `encode_seq(e, ..., lazy)`: the lazy copy,
  level 3's, is the one with the records for the deltas, so the wrappers `encode_emit()` and
  `encode_emit_rec()` are cut ([chapter 10](#ch-encoder--wrappers)). In `src/` the matcher has one
  more user, `seqlz_find()`.
- **What only the harness uses is cut**: all of `src/seqlz.h` but `seqlz_token()`, which goes into
  `seqlz_compress.c`, its one user there; `seqlz_find()`, `seqlz_encode()` and `encode_raw()`; the
  `all_symbols` field and its checks. The `raw` parameter of `code_literals()` stays: at level 3 it carries the deltas
  ([chapter 11](#ch-literals-enc--code-own)).

Every textual replacement goes through `must()` or `cut()`, which assert that the text occurs exactly as
often as expected, so a change in `src/` that moves such a text stops `port.py` instead of writing a copy
that silently missed a step.

### What port.py refuses

Before writing anything to disk, `port.py` checks every line it produced:

```python
{{ch15.port.harness_only}}
{{ch15.port.cpu_model}}
{{ch15.port.forbidden}}
```

`CPU_MODEL` applies to `lib/` only. The decoder once had a loop of its own for in-order cores, chosen by
the arm64 MIDR register; the review of the kernel copy
([#152](https://github.com/martinus/quetschn/issues/152)) replaced the two loops with one, and this check
keeps it that way. `FORBIDDEN` catches what a kernel reader cannot follow: the repository's documents
and tools, issue numbers, the project's names, and reasons that hold only for the project's machines,
such as the Mi 9T's 4.14 kernel. Since commit `6bff3de` the comments of `src/` give their reasons in plain
words, so few need a change. Those that do are rewritten through `REWRITES`, pairs of old and new text,
e.g. `"docs/format.md is the specification."` becomes
`"Documentation/staging/seqlz.rst specifies the format."`; a changed comment is wrapped again to 80
columns. A rewrite that matched nothing fails the run: its comment is gone, and the rewrite has to go
with it.

## include/linux/seqlz.h: two functions {#ch-zram-glue--interface}

@listing tools/kernel-port/seqlz.h 1-{{ch15.kport.seqlz_h}}

This is the shape of `include/linux/lz4.h`: work memory sizes as constants, functions that take the work
memory as a `void *`, and no tables. The interface speaks of blocks of `SEQLZ_BLOCK_SIZE` bytes, not of
`PAGE_SIZE`: one memory page with 4 KiB pages, a quarter of one with 16 KiB pages. Before commit `aaa381e` the kernel copy exported all of
`src/seqlz.h`, 7 functions and the code lengths. The comments are **kernel-doc**, the kernel's format
for documenting functions, which `scripts/kernel-doc` parses and checks.

- `Context: Any context` promises that the functions may run where sleeping is not allowed. zram calls
  them under a mutex and does not need it; it holds because the codec never sleeps or allocates.
- The `Return:` of `seqlz_compress()` has a subtlety of [chapter 10](#ch-encoder): the encoder stops as
  soon as fewer than {{ch15.enc_room}} bytes (`ENC_ROOM`) plus the next literals are free, so a page can
  be refused although its size with coded literals would have fit. With zram's 2 pages it never happens.
- "Safe for any input" is constraint C6 of the plan, the one hard limit
  ([chapter 1](#ch-zram--safety)).

@listing tools/kernel-port/api_compress.c 1-{{ch15.kport.api_compress}}

The compressor's 0 becomes `-E2BIG`, "output too small", and a level outside 1 to 3 is `-EINVAL`.
The `static_assert`s tie the interface's numbers to the codec's: the work memory is the matcher's
state, and the levels start at 1, which `seqlz_compress_block()` takes for granted.
`api_decompress.c` is the `seqlz_decompress()` of the diff above.

::: kernel EXPORT_SYMBOL_GPL
Modules are linked at load time against a table of exported symbols, and a function is in it only if
its file says `EXPORT_SYMBOL()` or `EXPORT_SYMBOL_GPL()`. The `_GPL` variant serves only modules whose
`MODULE_LICENSE()` is GPL-compatible. zram's backend can be in a zram module, so it needs the export.
:::

## The tables, generated when the kernel is built {#ch-zram-glue--init}

`seqlz_tables.c` is written into the build directory by `gen_seqlz_tables`, a program that the
kernel's build compiles for the build machine and runs, from the code lengths of
`lib/seqlz/seqlz_lengths.h` ([chapter 8](#ch-codes--kernel)). `port.py` writes that header with the
tree's own `scripts/gen-seqlz-lengths.py` from `lib/seqlz/seqlz_counts.txt`, so anybody can make it
again from the counts, and the patch carries counts, lengths and two generators, not
{{size.tables|grp}} bytes of tables. The result is `const struct seqlz_tables seqlz_tables`, the
kernel's one set, shared by every device and CPU: read-only data costs nothing to share, each core
caches its own copy of the lines, and a stray write faults instead of corrupting every page compressed
later. There is nothing to build at boot, so the module has no init and no exit function.

`CONFIG_SEQLZ` is a tristate, but the bool `CONFIG_ZRAM_BACKEND_SEQLZ` selects it, so it is built in
whenever the backend is enabled, also with `ZRAM=m`, as `ZRAM_BACKEND_LZ4` selects `LZ4_COMPRESS`.
The KUnit tests include `seqlz_lengths.h`, whose arrays are `static`, to check the code lengths
against the specification, so the kernel exports the two functions only.

## backend_seqlz.c: levels, and 8 KiB per CPU {#ch-zram-glue--backend}

The backend, `tools/kernel-port/backend_seqlz.c`, is modelled on zram's `backend_lz4.c` and is a
template for zram trees with and without the October 2026 series.

@listing tools/kernel-port/backend_seqlz.c {{ch15.backend.head.a}}-{{ch15.backend.head.b}}

`pr_fmt` comes before the first include: `pr_err()` and its relatives wrap their format string in it.
The `static_assert` says what zram's `Kconfig` makes sure of, `depends on PAGE_SIZE_4KB`: zram hands the
backend whole pages, and seqlz takes blocks of 4 KiB.
`ZCOMP_RW_SPLIT` is no kernel macro but a marker: `port.py` keeps the `#ifdef` parts if the tree's
`zcomp.h` has `struct zcomp_cstrm`, else the `#ifndef` parts, and removes the marker's lines.
`seqlz_release_params()` is empty but must exist, since zram calls it without a check for `NULL`. A
dictionary is refused with `-EOPNOTSUPP`, zram's "not set" level becomes level 2, and any other level
than 1 to 3 is `-EINVAL`, as `lz4` refuses invalid levels. The level is set per device:

```text
echo "algo=seqlz level=1" > /sys/block/zram0/algorithm_params
```

The contexts without the split, and `seqlz_destroy()`, which both forms share:

@listing tools/kernel-port/backend_seqlz.c {{ch15.backend.create.a}}-{{ch15.backend.create.b}}

What counts per CPU is what the allocator hands out ([chapter 1](#ch-zram--work-memory)). A struct of the
hash table and the scratch side by side was {{size.state}} + {{const.SCRATCH}} =
{{ch15.state_plus_scratch|grp}} bytes, above `KMALLOC_MAX_CACHE_SIZE`, and `kvzalloc()` took 16 KiB for it.
As a union it is {{size.state}} bytes from `kmalloc-8k`. `lz4` asks `vmalloc()` for 16 416 bytes, which is
5 pages, 20 480
([plan.md](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)):
12 KiB less per CPU, on the Mi 9T's 8 cores 96 KiB, resident whether the phone swaps or not.
`kzalloc_obj(*c)` is a newer typed spelling of `kzalloc(sizeof(*c), GFP_KERNEL)`, as zram's other
backends allocate their contexts; `GFP_KERNEL` may sleep, which is fine when a CPU comes up. Two
`static_assert`s at the top of the file keep `SEQLZ_MEM_COMPRESS` and `SEQLZ_MEM_DECOMPRESS` at most
`KMALLOC_MAX_CACHE_SIZE`: larger, `kzalloc()` would take pages of a higher order straight from the page
allocator, which can fail under fragmentation when a CPU comes up.
@assert const.SCRATCH <= size.state <= 8192 < ch15.state_plus_scratch <= 16384

::: why The union is safe because of zram's mutex
Commit `6063362` ([#154](https://github.com/martinus/quetschn/issues/154)) gives two reasons. zram holds
the stream's mutex for either operation, so they never run at the same time on one context. And neither
keeps anything from one page to the next: the matcher clears its table for every page, the decoder
writes its scratch before it reads it. The commit ran the kernel copy under KASAN, lockdep and UBSan: the corpus written and read back at the
two levels of then, 0 of 20 000 pages different; 10 minutes of swap with 4 workers per boot, 16.5
million pages out at level 2 and 16.9 million at level 1, 0 different.
:::

With the split, two functions and no union:

@listing tools/kernel-port/backend_seqlz.c {{ch15.backend.split.a}}-{{ch15.backend.split.b}}

Both contexts come from `kmalloc-8k`: 16 KiB per CPU at levels 2 and 3, 8 KiB at level 1, which has no
decompression context, and `kfree(NULL)` does nothing. The plan's score counts both, as work memory
per CPU.

@listing tools/kernel-port/backend_seqlz.c {{ch15.backend.pages.a}}-{{ch15.backend.pages.b}}

The harness's functions in the kernel's types: zram passes `2 * PAGE_SIZE` as `dst_len` to compress and
`PAGE_SIZE` to decompress, and the level comes from the params. In the union form `->cmem` and `->dmem`
are the same address; the member says which use is meant. Neither function prefetches: the VM's
measurements of the prefetch above used experimental code, `tools/zram-vm/zram-prefetch.patch` and the
VM's own backend. `port.py` adds `&backend_seqlz` to `zcomp.c`'s list, the entries to zram's `Kconfig`
and `Makefile`, and a paragraph to zram's documentation.

## prefetch() on x86-64: a patch to the kernel itself {#ch-zram-glue--prefetch-patch}

On x86-64 the kernel's `prefetch()` was `__builtin_prefetch()`, which clang drops under `-mno-sse`, so a
kernel built with clang, as Android's are, had no prefetches in the decoder
([chapter 5](#ch-compat--prefetch) has the story and the measurement). No inline assembly goes into
`lib/`, so the fix is a patch to the kernel, `tools/kernel-port/x86-prefetcht0.patch`, which defines
`ARCH_HAS_PREFETCH` for 64-bit in `arch/x86/include/asm/processor.h`:

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
with and without it. 32-bit x86 patches in `prefetchnta`, a non-temporal prefetch, on every CPU with SSE,
which would change what gcc builds get, so the patch does not use that. `port.py` applies it unless the
tree has it, and `src/seqlz_compat.h` does the same outside the kernel.

## Checks a submission needs {#ch-zram-glue--check}

`tools/kernel-port/check.sh <linux tree> [make args]` takes the tree's `HEAD` with `git archive`, so the
tree is never touched, commits the x86 patch on that base so that checkpatch sees seqlz's part alone,
runs `port.py`, and then:

1. configures `defconfig` with zram and the backend built in, seqlz and its KUnit tests as modules;
2. builds `lib/seqlz/` and `drivers/block/zram/` with `W=1`, the warnings the kernel leaves off by
   default because older code triggers them, and fails on any warning;
3. on x86-64, fails if `seqlz_decompress.o` has no `prefetcht0`, the check that the patch works with
   clang;
4. runs `checkpatch.pl --strict`, the style checker reviewers run before they read a patch, on the diff
   and on the x86 patch;
5. runs `scripts/kernel-doc -none -Wall -Werror` on the codec's files and the headers;
6. parses `Documentation/staging/seqlz.rst` with docutils, if Python has it;
7. compares the `seqlz_tables.c` that the build generated with the tables `src/` builds in userspace,
   printed the same way, and fails if a 64-bit division calls a library function, which the kernel
   does not have on 32-bit CPUs;
8. on arm64, builds the library and its tests again with 16 KiB and 64 KiB pages, with zram's backend
   off, as zram's `Kconfig` keeps it to 4 KiB pages.

CI runs it for x86-64 with gcc, and with clang for arm64, the phones' architecture, 32-bit arm, whose
32-bit `long` and pointers show a computation that assumes 64 bits, and s390, which is big-endian. The
32-bit kernel warns about stack frames above 1280 bytes, against 2048 on 64-bit. With gcc 16.2.1, x86-64
`defconfig` and the Makefile's `-O3`, the deepest frames are `seqlz_decode_literals()`, 640 bytes, and
`code_literals()`, 600, on 22nd October 2026, near 1000 with KASAN and UBSan. Since 11th October 2026 `check.sh` prints the
three deepest frames of each build and fails at 1024 bytes, the limit of older kernels' 32-bit
`FRAME_WARN`.

### KUnit tests in UML

**KUnit** is the kernel's unit test framework: test functions take a `struct kunit *`, check with macros
like `KUNIT_EXPECT_EQ()`, and run inside a kernel with its allocator and headers. **UML**, User Mode
Linux, is the kernel built as an ordinary program, `ARCH=um`, which boots in a second without a virtual
machine. `port.py` writes `lib/seqlz/tests/seqlz_kunit.c`, {{ch15.kunit.cases}} cases:

- `seqlz_test_tables`: the SHA-256 of each table's code lengths against `docs/format.md`. The template's
  placeholders, like `"@TOK_4k@"`, are filled from the specification's table, so the kernel's tests check
  against the specification, not a copy.
- `seqlz_test_example`: the specification's {{ab.len}} bytes that decode to a page of `ab`; the same bytes
  without the last one, and with a zero byte more, must be `-EINVAL`.
- `seqlz_test_round_trip`: generated pages of {{ch15.kunit.kinds}} kinds at every level.
- `seqlz_test_damaged`: damaged pages are rejected or decode, never out of bounds.
- `seqlz_test_one_sequence`: pages built by hand, each valid or invalid by one rule of the specification.
- `seqlz_test_coded_literals`: pages whose literals get coded.
- `seqlz_test_own_table`: pages with a table of their own, whose runs the decoder takes in one lookup
  of `quad[]` or alone, and tables that are invalid by one rule each.

The test's `decompress()` copies the input into an allocation of exactly its length, "so that KASAN sees
a read behind them": KASAN, the kernel's address sanitizer, reports a read past an allocation only where
the allocation ends. `tools/kernel-port/kunit.sh` runs `kunit.py run --kunitconfig=lib/seqlz` on such a
copy of the tree; CI runs it on x86-64 with KASAN and UBSan, and on arm64 with 64 KiB pages in qemu,
where a block is a sixteenth of a page. `tools/kernel-port/stress.sh`, run by hand,
boots the kernel copy in a VM with KASAN, lockdep, `DEBUG_ATOMIC_SLEEP` and UBSan, writes a corpus to zram
with `lz4` and seqlz at every level, swaps under memory pressure with every page compared, and runs the
kernel's zram selftests: the gate of the plan's Phase 5, and the source of the page counts above.

The specification goes along: `tools/kernel-port/format_rst.py` writes `docs/format.md` as
`Documentation/staging/seqlz.rst`. It handles only what `format.md` uses and fails on anything else, "so
that a new construct in format.md is noticed instead of turned into wrong reST". HTML comments mark text
only for the repository and text only for the kernel.

## Testing outside the kernel {#ch-zram-glue--tests}

`test/` has the doctest unit tests, one binary, `quetschn_test`. Two of them check this chapter's file.
`seqlz takes no dictionary, its tables are part of the format` gives the codecs a dictionary of the size
of `struct seqlz_lengths`, which once replaced the code lengths, and checks that a page compresses to the
same bytes as without. `seqlz compresses with the hash table, seqlz-2 decompresses with the
scratch` checks the context sizes, and that `seqlz-1` decodes with a stream without a context.

### Fuzzing

`fuzz/` has four targets for libFuzzer, which AFL++ runs too:

- `seqlz_decode_fuzz.c`: any input through `seqlz_decode()`, with and without the scratch, each buffer an
  allocation of exactly its size, so that AddressSanitizer sees one byte too far. A page that decodes
  without the scratch must decode the same with it.
- `seqlz_roundtrip_fuzz.c`: any page through `seqlz_compress()` and `seqlz_decode()`, at every level. It must come back, fit into 2 pages, and be the bytes `seqlz_encode()` writes for
  `seqlz_find()`'s sequences at that level.
- `seqlz_encode_fuzz.c`: any list of sequences through `seqlz_encode()`, also lists that make no page,
  added after an encoder overflow ([#121](https://github.com/martinus/quetschn/issues/121)) that the other
  targets could not reach, since the matcher's sequences are always valid.
- `seqlz_diff_fuzz.c`: the decoder against `tools/seqlz-ref/seqlz_ref.c`, a second decoder written from
  `docs/format.md` alone, "so that a misreading in src/ is not shared". It reads every bit on its own and
  checks every rule where the specification states it. Any disagreement on validity or on the page is a
  bug in one of them, or an unclear sentence of the specification.

`fuzz/smoke.sh` runs all four for 60 seconds each under AddressSanitizer and UndefinedBehaviorSanitizer;
CI runs it for 4 KiB and 16 KiB pages and once with MemorySanitizer, which reports a read of a byte the
codec did not write for this input. On the diff target, `docs/format.md` records two long AFL++ runs: on
6th October 2026, 10 instances for 2 hours, 1.7 billion inputs, and on 9th October, with the code of commit
`5342a9b`, 12 instances for 2 hours, 1.94 billion, no difference found in either
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#how-this-file-was-checked)).
ClusterFuzzLite runs the targets daily, an hour each with AddressSanitizer, UndefinedBehaviorSanitizer and
MemorySanitizer.

### The same bytes on a big-endian CPU

The format's multi-byte values are little endian, and the code reads and writes them with
`get_unaligned_le16()` and its relatives. Until commit `91fa53e` on 5th October 2026 it did not, and on
the big-endian s390x every page failed to round trip ([chapter 5](#ch-compat--endian)).
`test/seqlz_endian.c` keeps it fixed: 600 generated pages of six kinds, each compressed at every level
and decoded again, all compressed bytes hashed with FNV-1a. It is plain C, so it builds in
seconds under qemu. Its output on x86-64, and the same with `-m32`:

```text
{{ch15.endian.line}}
```

`coded {{ch15.endian.coded}}` counts the compressions, of {{ch15.endian.runs|grp}}, that came out with
coded literals, `own table {{ch15.endian.own}}` those among them with a table of the page's own. CI's
`same-bytes` job builds it for x86-64, 32-bit x86 and s390x in qemu and `diff`s the three lines.

### CI

| job | what it runs |
| --- | --- |
| `format` | `clang-format` 21.1.8 on every `.c`, `.h` and `.cpp` |
| `test` | `quetschn_test` built 4 ways: gcc debug with sanitizers, clang release, gcc release on arm64, gcc release with sanitizers for 16 KiB pages |
| `fuzz` | `fuzz/smoke.sh 60` for 4 KiB, 16 KiB and MSan |
| `same-bytes` | `test/seqlz_endian.c` on x86-64, 32-bit x86 and s390x, outputs diffed |
| `docs` | `tools/seqlz-ref/check.sh` and `tools/check-links.py` |
| `book` | this book, `docs/book/build.py --check` and `lint.py`; `pages` publishes it from `main` |
| `kernel-port` | `check.sh` for x86-64 (gcc), arm64, arm and s390 (clang), and `kunit.sh` with KASAN and UBSan, and on arm64 with 64 KiB pages |
| `ci-ok` | the one required check |

The `test` job greps doctest's summary for a non-zero number of passed assertions, because a binary that
ran no test also exits 0. `ci-ok` `needs` all the others and runs with `if: always()`, then checks that
each reported `success`: without `always()` a failed dependency would make it skipped, and GitHub counts a
skipped required check as passed. With one required job, the others can be renamed or split without
touching the branch settings.

::: warn What none of this covers
All these checks run on the CPUs and compilers CI has. The decode times and the memory come from separate
measurements, in the kernel VM and on the Mi 9T, which CI does not repeat. And the harness glue of this
chapter is not what the kernel runs: `backend_seqlz.c` is exercised by the KUnit tests and `stress.sh`,
and reached by the harness's benchmarks only with `PORT=1` in the VM.
:::
