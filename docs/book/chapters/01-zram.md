# What zram asks of a codec {#ch-zram}

A codec for zram compresses one page of memory at a time and gets it back when a program needs it.
zram changes what "good" means for such a codec. A program waits for every page it gets back, and
that page is in no CPU cache. zram does not pay for bytes but for slots of fixed sizes, so a few bytes
saved often save nothing. The codec's buffers exist once per CPU and are taken from every CPU for
good. And its decoder reads whatever is in RAM, also a page that was damaged. This chapter is zram
seen from the codec's side, and how the project scores a codec; the rest of the book refers back to
it.

## Pages, swap, and a swap device in RAM {#ch-zram--swap}

The kernel manages memory in **pages**, 4096 bytes on x86-64 and on most phones. When RAM runs
short, it **reclaims** pages that were not used for a while. A page of a file can be dropped, the
file still has it. A page of a program's heap or stack has to be written somewhere first, to
**swap**, traditionally a disk partition. When the program touches the page again, the CPU raises a
**page fault**, and the kernel reads the page back from swap into a fresh page of RAM: a swap-in.

**zram** is a block device whose storage is RAM: every page written to it is compressed and kept in
memory. As swap, that trades CPU time for memory, a few microseconds per page instead of a disk
access. Most Android phones and several Linux distributions, e.g. Fedora, swap to zram. The project's
numbers come from two such machines, a desktop with a Ryzen 9 7950X and a Xiaomi Mi 9T phone, a fast
x86-64 core and a phone's small arm64 core.

Every page is compressed on its own, so that any one page can be read back alone. That is the first
fact that shapes seqlz: a compressor has only 4096 bytes to find repeats in and to learn from, and a
format that starts each input with a table of its own codes, as `zstd` does, spends a good part of a
page on the table ([chapter 2](#ch-compression--formats)).

## The path of a page {#ch-zram--path}

The names are zram's functions in mainline Linux at `986c24e0fe44`, September 2026, the kernel the
project checked against.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 330" role="img" aria-label="A page goes from a program through reclaim and zram's compression into a zsmalloc slot; on a page fault it comes back through decompression while the program waits.">
  <text x="10" y="18" font-size="12">swap-out: often in the background (kswapd), sometimes a task in direct reclaim waits</text>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="30" width="124" height="60" rx="4" class="fill-sheet"/>
    <rect x="154" y="30" width="124" height="60" rx="4" class="fill-sheet"/>
    <rect x="298" y="30" width="124" height="60" rx="4" class="fill-sheet"/>
    <rect x="442" y="30" width="124" height="60" rx="4" class="fill-sheet"/>
    <rect x="586" y="30" width="124" height="60" rx="4" class="fill-sheet"/>
    <line x1="134" y1="60" x2="150" y2="60"/><path d="M150,56 L154,60 L150,64" />
    <line x1="278" y1="60" x2="294" y2="60"/><path d="M294,56 L298,60 L294,64" />
    <line x1="422" y1="60" x2="438" y2="60"/><path d="M438,56 L442,60 L438,64" />
    <line x1="566" y1="60" x2="582" y2="60"/><path d="M582,56 L586,60 L582,64" />
  </g>
  <g font-size="11" text-anchor="middle">
    <text x="72" y="50">page of a program</text><text x="72" y="66">4096 bytes,</text><text x="72" y="80">not used lately</text>
    <text x="216" y="50">reclaim</text><text x="216" y="66">picks the page,</text><text x="216" y="80">writes it to swap</text>
    <text x="360" y="50">zram_write_page()</text><text x="360" y="66">one value only?</text><text x="360" y="80">keep the value</text>
    <text x="504" y="50">the codec</text><text x="504" y="66">compresses into</text><text x="504" y="80">a 2-page buffer</text>
    <text x="648" y="50">zs_malloc()</text><text x="648" y="66">a slot of the</text><text x="648" y="80">size class</text>
  </g>
  <g font-size="11">
    <line x1="504" y1="90" x2="504" y2="104" stroke="currentColor" fill="none"/>
    <text x="504" y="117" text-anchor="middle">3625 bytes or more: stored raw, 4096</text>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <line x1="648" y1="90" x2="648" y2="140"/><path d="M644,136 L648,140 L652,136"/>
    <rect x="154" y="140" width="556" height="40" class="fill-paper"/>
    <rect x="160" y="148" width="40" height="24" class="fill-sheet"/>
    <rect x="204" y="148" width="72" height="24" class="fill-sheet"/>
    <rect x="280" y="148" width="28" height="24" class="fill-sheet"/>
    <rect x="312" y="148" width="96" height="24" class="fill-sheet"/>
    <rect x="412" y="148" width="56" height="24" class="fill-sheet"/>
    <rect x="472" y="148" width="40" height="24" class="fill-sheet"/>
    <rect x="516" y="148" width="64" height="24" class="fill-sheet"/>
    <rect x="584" y="148" width="40" height="24" class="fill-acc" fill-opacity="0.35"/>
    <rect x="628" y="148" width="76" height="24" class="fill-sheet"/>
  </g>
  <text x="10" y="158" font-size="11">zsmalloc pool,</text>
  <text x="10" y="172" font-size="11">compressed pages</text>
  <g class="acc">
    <g fill="none" stroke="currentColor" stroke-width="1.6">
      <line x1="504" y1="180" x2="504" y2="206"/><path d="M500,202 L504,206 L508,202"/>
      <rect x="10" y="210" width="124" height="60" rx="4" class="fill-sheet"/>
      <rect x="154" y="210" width="124" height="60" rx="4" class="fill-sheet"/>
      <rect x="298" y="210" width="124" height="60" rx="4" class="fill-sheet"/>
      <rect x="442" y="210" width="124" height="60" rx="4" class="fill-sheet"/>
      <rect x="586" y="210" width="124" height="60" rx="4" class="fill-sheet"/>
      <line x1="134" y1="240" x2="150" y2="240"/><path d="M150,236 L154,240 L150,244"/>
      <line x1="278" y1="240" x2="294" y2="240"/><path d="M294,236 L298,240 L294,244"/>
      <line x1="422" y1="240" x2="438" y2="240"/><path d="M438,236 L442,240 L438,244"/>
      <line x1="566" y1="240" x2="582" y2="240"/><path d="M582,236 L586,240 L582,244"/>
    </g>
    <g font-size="11" text-anchor="middle">
      <text x="72" y="230">the program</text><text x="72" y="246">touches the page:</text><text x="72" y="260">a page fault</text>
      <text x="216" y="230">swap-in</text><text x="216" y="246">the kernel asks</text><text x="216" y="260">zram for it</text>
      <text x="360" y="230">zram_read_page()</text><text x="360" y="246">one value or raw:</text><text x="360" y="260">no codec</text>
      <text x="504" y="230">read the slot</text><text x="504" y="246">compressed bytes,</text><text x="504" y="260">mostly cold</text>
      <text x="648" y="230">the codec</text><text x="648" y="246">decompresses into</text><text x="648" y="260">the page, done</text>
    </g>
    <text x="10" y="294" font-size="12">swap-in: the task that touched the page waits for every step of this row</text>
  </g>
  <text x="10" y="318" font-size="11">Names as in drivers/block/zram/zram_drv.c at 986c24e0fe44.</text>
</svg>
</div>
<figcaption>A page's way into zram and back: writing is often done in the background, reading always with a program waiting.</figcaption>
</figure>

**Swap-out.** `zram_write_page()` first checks with `page_same_filled()` whether the page is one
8-byte value repeated, e.g. all zeros; such a page is kept as that value, without a codec. For every
other page zram takes this CPU's **stream**, a struct with a mutex, a buffer of 2 pages and the
codec's work memory, and calls the codec through `zcomp_compress()`. A result of 3625 bytes or more is
thrown away and the page stored as it is; anything shorter goes into a slot from the allocator,
zsmalloc.

**Swap-in.** zram looks up the slot. A same-filled page is filled with its value, a raw page copied,
and only the rest reaches `zcomp_decompress()`, with the compressed bytes and the page to write.

**Who waits.** Most swap-outs are done by `kswapd`, a kernel thread that reclaims in the background.
When memory runs out faster than it frees, a task that wants memory reclaims itself, **direct
reclaim**, and waits for the compression too. A swap-in is always done inside the page fault of the
task that touched the page, which is stopped until the page is back. So reading is always on the
critical path, writing sometimes.

**Cold.** A swapped-out page was not used for a while, so its compressed bytes are in no CPU cache
when the fault comes, nor is the page the codec writes into. Each access goes to RAM, about 100 ns on
a desktop and more on a phone. A benchmark that decodes the same page in a loop measures something no
swap-in sees, so the project measures **cold reads**, with the compressed bytes flushed from the
cache first, and in the end the whole page fault in a virtual machine and on the phone
([`docs/plan.md` §3.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#32-decompression-is-a-tail-latency-problem-not-a-throughput-problem)).

::: measured How much of a swap-in is the codec
In a virtual machine on the Ryzen 9 7950X, with pages the desktop swapped into zram, a cold swap-in
with `lz4` takes 3.51 µs, of which 1.43 µs are `zcomp_decompress()`; with `seqlz-2` 4.13 µs, of
which 2.04 µs are the codec. The kernel's part, the fault, the swap entry and zram's slot, is about
1.9 to 2.0 µs for every codec, so a codec 43% slower than `lz4` makes a swap-in 18% slower
([explored-designs.md, "The whole page fault"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-whole-page-fault-the-kernels-part-is-the-same-for-every-codec-the-gap-to-lz4-about-halves)).
:::

## zsmalloc: bytes saved count only across a size class {#ch-zram--size-classes}

**zsmalloc** does not hand out any number of bytes. It has **size classes**, nominal slots in steps
of 16 bytes on 4 KiB pages (`ZS_SIZE_CLASS_DELTA`), and an object of $n$ bytes goes into the smallest
class that holds $n + 8$, for the handle (`ZS_HANDLE_SIZE`). Each class packs its slots into a
"zspage" of 1 to 8 physical pages, chosen to waste as little as possible, and classes with the same
packing are merged into the larger one. The project's C++ port of that setup,
`bench/zsmalloc_cost.cpp`, reproduces the kernel's classes: after merging, 255 nominal classes are
119 different ones
([`docs/plan.md` §3.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#31-zram-does-not-pay-for-bytes-it-pays-for-zsmalloc-size-classes)).

So what a page costs is a step function of its compressed length. Around 1 KiB the classes are 32
bytes apart:

| compressed length | slot | memory per page, with the zspage's waste |
| --- | --- | --- |
| 969 to 984 bytes | 992 | 993.0 |
| 985 to 1016 bytes | 1024 | 1024.0 |
| 1017 to 1048 bytes | 1056 | 1057.0 |

Saving 31 bytes on a page of 1048 bytes saves nothing; saving the one byte from 1017 to 1016 saves
33. The steps grow with the size: 17 bytes on average below 1 KiB, 28 between 1 and 2 KiB, 68 between
2 and 3 KiB, and 93 from 3 KiB on, up to 144. At 3625 bytes there is a cliff: zsmalloc's largest
classes hold one object per zspage, no better than a whole page, so zram stores every page that
compresses to `huge_class_size`, 3625 bytes, or more as 4096 bytes.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 240" role="img" aria-label="Two staircases: the slot size against the compressed length, around 1 KiB in steps of 16 and 32 bytes, and from 3300 bytes to the cliff at 3625 where every page takes 4096 bytes.">
  <g fill="none" stroke="currentColor" stroke-width="1">
    <line x1="60" y1="190" x2="340" y2="190"/><line x1="60" y1="190" x2="60" y2="40"/>
    <line x1="420" y1="190" x2="700" y2="190"/><line x1="420" y1="190" x2="420" y2="40"/>
  </g>
  <g class="mut" fill="none" stroke="currentColor" stroke-width="1" stroke-dasharray="3 3">
    <line x1="60" y1="182.1" x2="340" y2="63.7"/>
    <line x1="420" y1="190" x2="700" y2="44.4"/>
  </g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="2">
    <path d="M60,174.2 H64.7 V161.6 H94.5 V148.9 H124.4 V123.7 H184.1 V98.4 H243.9 V85.8 H273.7 V73.2 H303.6 V60.5 H333.5 V47.9 H340"/>
    <path d="M420,170.2 H455.4 V152.7 H489.1 V138.0 H517.3 V129.3 H534.1 V44.4 H700"/>
  </g>
  <g font-size="11">
    <text x="60" y="206" text-anchor="middle">950</text><text x="153.3" y="206" text-anchor="middle">1000</text>
    <text x="246.7" y="206" text-anchor="middle">1050</text><text x="340" y="206" text-anchor="middle">1100</text>
    <text x="54" y="185" text-anchor="end">950</text><text x="54" y="146" text-anchor="end">1000</text>
    <text x="54" y="107" text-anchor="end">1050</text><text x="54" y="67" text-anchor="end">1100</text>
    <text x="130" y="118">1024</text><text x="190" y="93">1056</text>
    <text x="420" y="206" text-anchor="middle">3300</text><text x="534.3" y="206" text-anchor="middle">3625</text>
    <text x="700" y="206" text-anchor="middle">4096</text>
    <text x="414" y="175" text-anchor="end">3400</text><text x="414" y="133" text-anchor="end">3632</text>
    <text x="414" y="48" text-anchor="end">4096</text>
    <text x="558" y="144">the cliff: 3625 bytes</text><text x="558" y="158">or more are stored</text><text x="558" y="172">as 4096</text>
    <text x="200" y="228" text-anchor="middle">compressed length, bytes</text>
    <text x="560" y="228" text-anchor="middle">compressed length, bytes</text>
    <text x="60" y="30">slot, bytes</text><text x="420" y="30">slot, bytes</text>
    <text x="190" y="178" class="mut">dashed: slot = length</text>
  </g>
</svg>
</div>
<figcaption>The slot zsmalloc gives a page against its compressed length, 4 KiB pages, 64-bit kernel, CONFIG_ZSMALLOC_CHAIN_SIZE=8; computed with the project's port of zsmalloc's class setup.</figcaption>
</figure>

Three things follow, and the book comes back to each:

- **A few bytes per page are cheap.** A format can afford a byte of header, a flag per page, a field
  that makes the decoder faster. Coding the literals pays only if it saves $\frac{1}{16}$ of them and
  {{ch01.LIT_CODED_MIN}} bytes more ([chapter 11](#ch-literals-enc)).
- **The cliff is the largest step.** A page moved from 3625 to 3600 bytes saves 455 bytes, 18 times
  the 25 bytes of compression it took. And the decoder only ever sees pages below 3625 bytes.
- **The measure of size is not the mean ratio.** The project counts the **stored size**, the sum of
  what zsmalloc spends on the pages, "Σ zsmalloc cost". On a new zram device that model equals zram's
  own `mem_used_total` to the byte
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zrams-memory-against-the-model-exact-on-a-new-device-σ-zsmalloc-cost-035-to-047-below-it)).

The project's metrics leave out same-filled pages
([`docs/plan.md` §5.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#52-metrics)): they
cost every codec the same and would make all codecs look better without telling them apart. A few
measurements quoted later, the swap times in the kernel VM among them, include them; where that
matters, the text says so.

## What zram calls: a backend's five functions {#ch-zram--zcomp}

Since 2024 zram has its own interface for codecs, a **backend**, in `drivers/block/zram/zcomp.h`
(commit `917a59e81c34`). This is the kernel's struct, not part of `src/`:

```c
struct zcomp_ops {
	int (*compress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req);
	int (*decompress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req);

	int (*create_ctx)(struct zcomp_params *params, struct zcomp_ctx *ctx);
	void (*destroy_ctx)(struct zcomp_ctx *ctx);

	int (*setup_params)(struct zcomp_params *params);
	void (*release_params)(struct zcomp_params *params);

	const char *name;
};
```

- `setup_params()` runs once per zram device. `struct zcomp_params` has the compression **level**,
  an optional **dictionary** and `drv_data` for the backend's data that all CPUs share. seqlz has three
  levels: 1 stores the literals of a page as they are (`seqlz-1`), 2 codes them with one of 8 fixed
  tables (`seqlz-2`, the default), 3 also searches a little longer for matches, may give a page a
  literal table of its own and stores the literals as deltas to earlier bytes of the page where that pays
  (`seqlz-3`). Its tables are fixed and part of the format, so the kernel
  backend refuses a dictionary with `-EOPNOTSUPP` ([chapter 15](#ch-zram-glue)).
- `create_ctx()` runs once per CPU and allocates the codec's work memory, `void *context`, never
  while a page is compressed.
- `compress()` gets the page in `src`, always `PAGE_SIZE` bytes, and the stream's buffer of
  `2 * PAGE_SIZE` in `dst`, and sets `dst_len` to the length it wrote.
- `decompress()` gets the compressed bytes and the length zram stored. The compressed page carries
  no length of its own and no checksum; that length is all the decoder knows about where its input
  ends.

zram holds the stream's mutex for the whole call, so one context is never used by two calls at once.
A series of Sergey Senozhatsky of 5th October 2026, not merged yet, splits the stream into one for
writing and one for reading, with `create_cctx()` and an optional `create_dctx()`. The project's
kernel backend is written for both forms
([`docs/plan.md` §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)).

The benchmarks call seqlz through a copy of this interface in C, `src/zram_seqlz.c`
([chapter 15](#ch-zram-glue)). Its decompress function is the whole contract:

@listing src/zram_seqlz.c 123-133

`quetschn_prefetch_page()` asks the CPU to start loading the compressed bytes and the output page
before the decoder needs them ([chapter 3](#ch-cpu--prefetch)); it stands for what the project's
experiments let zram do before every decompression. `p->drv_data` are the tables built once per
device, `s->context` this CPU's buffer for the decoded literals.

## Work memory is per CPU, and counted as allocated {#ch-zram--work-memory}

::: kernel Per-CPU data, kmalloc() and vmalloc()
Data kept once per CPU, **per-CPU data**, lets code on one CPU use its copy without a lock. zram's
streams are such data: a machine with 8 CPUs has 8 streams, each with its own buffer and codec
context, allocated when the device is set up and kept as long as it exists, whether that CPU ever
swaps or not.

`kmalloc()` hands out physically contiguous memory from caches of powers of 2, up to
`KMALLOC_MAX_CACHE_SIZE`, 8 KiB with 4 KiB pages: a request of {{const.SCRATCH}} bytes gets
{{ch01.dctx_kmalloc}} bytes. A larger request gets a power of 2 of whole pages: 61 440 bytes get 16 pages,
65 536 bytes. `vmalloc()` maps whole pages into a contiguous range of addresses: 16 416 bytes take 5
pages, 20 480 bytes. `kvmalloc()` tries the first and falls back to the second.
:::

zram allocates 3 pages per CPU for every codec, the buffer of 2 pages and a page `local_copy` for
an object that zsmalloc split over two pages. The codec's context comes on top:

| codec | work memory asked for, per CPU | what the allocator hands out |
| --- | --- | --- |
| `lzo`, `lzo-rle` | 16 384 bytes, `kzalloc()` | 16 384 bytes |
| `lz4` | 16 416 bytes, `vmalloc()` | 20 480 bytes, 5 pages |
| `842` | 61 440 bytes, `kmalloc()` | 65 536 bytes, 16 pages |
| `zstd` 3 | 186 112 bytes, `vzalloc()`, for compression and decompression | 192 544 bytes, 47 pages and a struct |

The plan calls this memory **C5**, and it is a cost: on the test phone, with 8 cores, every 4 KiB of
work memory per CPU is 32 KiB of RAM that is never free, on the device where RAM is shortest. The score
counts it, spread over the pages in zram: with 1 GiB in zram those 32 KiB are 0.1 bytes per page, with
64 MiB 2 bytes
([`docs/plan.md` §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)).
seqlz needs {{size.state}} bytes for the matcher's table while it compresses and {{const.SCRATCH}}
bytes for the decoded literals while it decompresses. zram holds the stream's mutex for either, and
neither keeps anything from one page to the next, so the kernel backend puts both into one union of {{ch01.union}} bytes, which `kmalloc()` hands out as
exactly {{ch01.union_kib}} KiB. With the split streams they are two allocations,
{{ch01.cctx_kib}} KiB each after rounding, {{ch01.split_kib}} KiB per CPU, below `lz4`'s 20
KiB. The tables, {{size.tables|grp}} bytes, are not per CPU: they are built once when the kernel
starts.
@assert ch01.union == 1024 * ch01.union_kib and ch01.cctx_kib == ch01.dctx_kib and ch01.split_kib < 20
[Chapter 6](#ch-header) and [chapter 15](#ch-zram-glue) have the details, and why 16 KiB pages need
twice `lz4`'s work memory.

## The decoder must survive any input {#ch-zram--safety}

zram stores no checksum. A bit can flip in RAM, a bug elsewhere in the kernel can write over memory
zsmalloc owns, and with zram's writeback a page goes to a disk and comes back. A decoder that then
reads past its input or writes past the page corrupts the kernel's memory.

So the project's second hard limit, **C6**: the decompressor is safe for any input and bounded in
time. Whatever bytes it gets, it reads only its input and its tables, writes only the output page and
its scratch, and returns after a fixed amount of work at most, with an error for an invalid page. zram
then fails the read; one page is lost, the kernel is not.

The reason C6 is a hard limit is history. The last new codec proposed for zram, zBeWalgo in 2018,
reached version 7 of its patch series and was never merged. One objection on the mailing list, in
Eric Biggers's words: *"You cannot just ignore fuzz-safety in the decompressor either"*
([`docs/plan.md` §2.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#22-the-failure-zbewalgo-benjamin-warnke-2018-reached-v7-never-merged)).
seqlz's decoder checks every length and offset against the room left before it uses them, stops
after at most `PAGE / 4 + 1` sequences, and requires that a page ends exactly where its bits end
([chapter 14](#ch-decode)). Fuzzers, programs that feed a function random and damaged inputs and
report every crash and every access out of bounds ([chapter 15](#ch-zram-glue--fuzzing)), fed it
billions of pages. On the Mi 9T's small core, a Cortex-A55, the slowest page the project built to
be slow decodes in 1.33 times the p99 time of real pages; for `lz4` the same is 1.57
([explored-designs.md, "The worst case"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-worst-case-the-slowest-pages-found-cost-13-times-the-p99-of-real-ones-as-for-lz4)).

## What kernel code cannot do {#ch-zram--kernel-rules}

- **No C library.** The kernel has its own `memcpy()` and `memset()` and its own type names, `u8` to
  `u64`. seqlz allocates nothing: zram passes in the work memory.
- **No floating point and no vector registers.** The kernel does not save a program's floating point
  and SIMD registers when it is entered, so it is compiled with `-mno-sse` on x86-64 and
  `-mgeneral-regs-only` on arm64. `kernel_fpu_begin()` borrows them, at a cost and not in every
  context. Every compressor in the kernel is scalar code, and so is seqlz.
- **A small stack.** A kernel thread has 16 KiB of stack on x86-64 and arm64. The build warns about a
  frame above `CONFIG_FRAME_WARN`, 2048 bytes on 64-bit and 1280 on most 32-bit builds in the kernel
  of September 2026, 1024 in older ones. Every frame of seqlz's kernel copy stays below 1024; the
  deepest, `seqlz_code_literals()` and `seqlz_decode_literals()`, are 640 bytes with gcc for x86-64
  ([chapter 15](#ch-zram-glue--check)).
- **No sleeping in compress and decompress.** zram called the codec with preemption disabled, older
  kernels such as the phone's 4.14 still do, and a fix proposed in August 2026 would bring that back
  for backends like seqlz. So no allocation, no lock, no page fault in the codec
  ([`docs/plan.md` §3.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#32-decompression-is-a-tail-latency-problem-not-a-throughput-problem)).
- **Every architecture.** The same source runs on little-endian x86-64 and arm64, on big-endian s390
  and on 32-bit arm, and CI checks that x86-64, 32-bit x86 and s390x write the same compressed bytes
  for the same pages ([chapter 5](#ch-compat--endian)).
- **No inline assembly, nothing that depends on the CPU model.** A maintainer of `lib/` reads one C
  source for all machines. A gap in the kernel is fixed in the kernel, with a patch of its own;
  [chapter 5](#ch-compat--prefetch) shows the one seqlz needs.

`src/seqlz_compat.h` supplies the kernel's names outside the kernel, so that the benchmarks, the tests
and the fuzzers build the same code ([chapter 5](#ch-compat)).

## The score: bytes against time {#ch-zram--score}

No codec stores pages smallest and handles them fastest. The project's first plan had bars, e.g. a
p99 time no worse than `lz4`'s, and bars turned out to be cliffs: a design that saved 4% of memory
failed because its p99 was 1 µs over the bar, without asking what 1 µs is worth. So designs are
judged by a **score**
([`docs/plan.md` §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)):

- **bytes**: the stored size per page, what zsmalloc spends.
- **time**: per page written to zram, $\text{write} + r \times \text{read}$, each the **mean** over all pages, in µs.
- **score**: $\text{bytes} + \lambda \times \text{time}$, lower is better.

**The mean**, because it is the total time spent on the pages: a program that comes back after a
while faults in many pages one after the other and waits for the sum. The 99th percentile, p99, stays
a guard: a p99 that gets clearly worse means some pages are pathological, and they are looked for.

**$r = 0.73$** is how many reads come per write, on a phone. On the project's Pixel 9a in daily use,
`pswpin` and `pswpout` in `/proc/vmstat` counted 78 335 824 pages swapped in against 106 938 781
swapped out in 7.1 days, 16th October 2026; zram is for phones, so the score uses that. The project's
desktop, with zram as its only swap, had 0.34 in 27.5 days. A page written is read back at most once,
often never, because it is freed first.

**lambda** is an exchange rate: how many bytes of stored size one µs per page is worth to you. On a
phone short of RAM a byte is worth more than on a server with memory to spare, so the project does not
pick one. It plots each codec at (time, bytes) and draws the lower left convex hull, the codecs that
have the lowest score for some lambda; the slope between two neighbours on the hull is the exchange
rate at which the faster one starts to win. `quetschn-score` computes it from the benchmark logs.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 360" role="img" aria-label="Stored bytes per page against time per page written for lz4, lzo-rle, seqlz-2 and zstd 3, means over the pages of a desktop's zram, with the lower left hull from lz4 through lzo-rle and seqlz-2 to zstd 3.">
  <g fill="none" stroke="currentColor" stroke-width="1">
    <line x1="70" y1="320" x2="690" y2="320"/><line x1="70" y1="320" x2="70" y2="30"/>
  </g>
  <g class="mut" fill="none" stroke="currentColor" stroke-width="0.6">
    <line x1="194" y1="320" x2="194" y2="30"/><line x1="318" y1="320" x2="318" y2="30"/>
    <line x1="442" y1="320" x2="442" y2="30"/><line x1="566" y1="320" x2="566" y2="30"/>
    <line x1="690" y1="320" x2="690" y2="30"/>
    <line x1="70" y1="247.5" x2="690" y2="247.5"/><line x1="70" y1="175" x2="690" y2="175"/>
    <line x1="70" y1="102.5" x2="690" y2="102.5"/><line x1="70" y1="30" x2="690" y2="30"/>
  </g>
  <g font-size="11">
    <text x="70" y="336" text-anchor="middle">0</text><text x="194" y="336" text-anchor="middle">4</text>
    <text x="318" y="336" text-anchor="middle">8</text><text x="442" y="336" text-anchor="middle">12</text>
    <text x="566" y="336" text-anchor="middle">16</text><text x="690" y="336" text-anchor="middle">20</text>
    <text x="64" y="324" text-anchor="end">0</text><text x="64" y="251.5" text-anchor="end">500</text>
    <text x="64" y="179" text-anchor="end">1000</text><text x="64" y="106.5" text-anchor="end">1500</text>
    <text x="64" y="34" text-anchor="end">2000</text>
    <text x="380" y="354" text-anchor="middle">time per page written, write + 0.73 × cold read, µs</text>
    <text x="70" y="20">stored bytes per page</text>
  </g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="2" stroke-dasharray="6 4">
    <path d="M301.0,87.7 L303.4,99.6 L336.3,153.4 L629.2,159.8"/>
  </g>
  <g fill="currentColor">
    <circle cx="301.0" cy="87.7" r="4.5"/>
    <circle cx="303.4" cy="99.6" r="4.5"/>
    <circle cx="629.2" cy="159.8" r="4.5"/>
  </g>
  <g class="acc"><circle cx="336.3" cy="153.4" r="5.5" fill="currentColor"/></g>
  <g font-size="11" class="halo">
    <text x="309" y="84">lz4, 7.45 µs, 1602 B</text>
    <text x="294" y="117" text-anchor="end">lzo-rle, 7.53 µs, 1520 B</text>
    <text x="345" y="175">seqlz-2, 8.59 µs, 1149 B</text>
    <text x="690" y="146" text-anchor="end">zstd 3, 18.04 µs, 1105 B</text>
    <text x="324" y="128" class="acc">350 B/µs</text>
    <text x="460" y="150" class="acc">4.7 B/µs</text>
  </g>
</svg>
</div>
<figcaption>The four codecs of the table below, means over the pages; the dashed line is the hull, and the slopes are the exchange rates between its neighbours.</figcaption>
</figure>

Read the figure from the right, with the numbers of the table in the next section, measured on 14th
October 2026. Going from `zstd` 3 to `seqlz-2` saves 9.45 µs per page written and costs 44
bytes, 4.7 bytes per µs: whoever values a µs at less should run `zstd`. Going from `seqlz-2` to
`lzo-rle` saves 1.06 µs and costs 371 bytes, 350 bytes per µs: whoever values a µs at more should run
`lzo-rle`, or above 1025 bytes per µs `lz4`, 0.08 µs faster for 82 bytes more. In between,
`seqlz-2` has the lowest score.

With the times of a real swap-out and swap-in in the virtual machine, all three levels in the same
boots, the means over four zram dumps of a phone and a desktop, 16th October 2026, the hull is `lz4`,
`seqlz-1`, `seqlz-2`, `seqlz-3` and `zstd` 3, at 597, 145, 21 and 0.2 bytes per µs: `seqlz-2` has the
lowest score from 21 to 145 bytes per µs. `lzo-rle` is not on it, its swap-ins are slower than `lz4`'s
([explored-designs.md, "The numbers again on 16th October"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-on-16th-october-all-three-levels-with-lzo-rle-the-readmes-chart)).
Whoever runs `lz4` instead of `zstd` 3 says, by that choice, that a µs is worth more than about 43
bytes to them, 457 bytes per page for 10.6 µs.

## The codecs zram has, and where seqlz sits {#ch-zram--codecs}

Mainline zram has seven backends: `lzo-rle`, the default, `lzo`, `lz4`, `lz4hc`, `zstd`, `deflate` and
`842`. Three matter here. `lz4` is the fast one, the common choice on Android. `lzo-rle` is `lzo` with a
shortcut for runs of zeros, merged in 2019. `zstd` stores pages smallest and takes about twice the
time. [Chapter 2](#ch-compression--formats) shows how `lz4`, `zstd` and seqlz write a page down.

Measured in the real kernel in a virtual machine (`tools/zram-vm/run.sh`), on one CPU of the Ryzen 9
7950X fixed at 4.5 GHz, on 40 000 pages the desktop swapped into zram in September 2026. A program in
the VM writes each page with `pwrite()` and reads it back with `pread()`, so each time is the whole
system call, zram and zsmalloc included, the median of 3 runs; for a cold read the compressed page is
flushed from the CPU caches first. Means over the pages and over 3 boots, 14th October 2026
([`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-result-lz4s-time-zstds-size-almost)):

| codec | stored bytes per page | write, µs | cold read, µs | time per page written, µs |
| --- | --- | --- | --- | --- |
| `lz4` | 1602 | 5.52 | 2.64 | 7.45 |
| `lzo-rle` | 1520 | 5.41 | 2.91 | 7.53 |
| **`seqlz-2`** | **1149** | **6.48** | **2.89** | **8.59** |
| `zstd` 3 | 1105 | 13.96 | 5.59 | 18.04 |

In these runs of 14th October 2026, `seqlz-2` stores pages 24% smaller than `lzo-rle` and 28%
smaller than `lz4`, 4% larger than `zstd` 3, for 17% more time than `lz4` to write a page.

::: warn The read times in this table flatter the decoder
The read benchmark reads every page several times in a row, warm and cold, and that trains the CPU's
branch predictor on the page ([chapter 3](#ch-cpu--branches)). In a real swap-in, which decodes a page
once, `seqlz-2` mispredicts about 110 branches per page more, and its reads are 0.62 µs slower
than `lz4`'s instead of the 0.25 µs above
([explored-designs.md, "The decoder in a fault, found"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
The hull of the swap times above has this in it. On the phone, in its own Linux 4.14 kernel,
`seqlz-2` stores pages 28% smaller than `lz4` and swaps them in 11% to 13% slower warm and 17%
to 25% slower cold
([`docs/plan.md`](https://github.com/martinus/quetschn/blob/main/docs/plan.md), 9th October 2026).
:::

## The goal, its costs and its one limit {#ch-zram--goal}

The goal is a codec merged into mainline Linux as a zram backend because it is measurably better
than what is there. No small scalar codec without allocation beats `zstd`'s entropy coder on size
([chapter 2](#ch-compression--entropy)), so the target is the fast corner: as close as possible to
`lz4`'s speed and to `zstd`'s size. It
might be unreachable; a design is judged by how much closer it gets
([`docs/plan.md` §1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria)).

The plan writes the merge argument as six criteria. Their status on 9th October 2026, means over the
dumps:

| | criterion | status, as in [`docs/plan.md` §1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria) |
| --- | --- | --- |
| C1 | at least 8% less Σ zsmalloc cost than the better of `lzo-rle` and `lz4` | met: 22% less than `lzo-rle` on the desktop, 28% less than `lz4` on the phone |
| C2 | the p99 cold read time against `lz4`, on x86-64 and on an arm64 little core | slower |
| C3 | the p99 write time against `lz4` | 1.20 times |
| C4 | beats `lz4` with a trained dictionary too | met: 20.4% less |
| C5 | work memory per CPU | 8 KiB with 4 KiB pages, 16 KiB with the contexts split |
| C6 | the decoder safe for any input and bounded in time | met |

C6 is the only hard limit: an unsafe decoder is not a trade-off. C5 is a cost, counted in the score's
bytes for a named machine, e.g. 8 CPUs and 1 GiB in zram. C1 and C4 are reported. C2 and C3 are
guards, not gates: a design is never dropped because its p99 misses the bar, and never kept because it
meets it; the score decides. Since C2 does not hold, the argument for a merge is memory at a small
cost in time, not a faster read.

**No recompression.** zram can compress idle pages a second time with a slower algorithm. The project
measured that with `zstd` 3 and with an optimal parser for seqlz's format, and did not pursue it
([chapter 17](#ch-decisions--not-kept)). Every design in this book compresses a page once.

$r = 0.73$ is one phone's, in one week of one person's use. Its swap-ins come in bursts: in 15 minutes
of use on 16th October 2026, 92% of the pages came in bursts of 1024 pages and more, and 114 of 366 241
came alone ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#swap-ins-on-a-pixel-9a-in-use-92-of-the-pages-in-bursts-of-1024-or-more-073-reads-per-write-again)). So a slower read costs a user thousands of times
the difference per page, which is what the mean counts; how long a burst takes, and on which core, is not
measured. With 16 KiB pages, which Android is moving to and which the project
has only from the Android 17 emulator, seqlz needs more work memory than `lz4` once the streams are
split, which the score counts as a cost.
@assert k16.size.state + k16.const.SCRATCH > 16416 and k16.size.state <= 16416
