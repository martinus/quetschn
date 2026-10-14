# What zram asks of a codec {#ch-zram}

A codec for zram compresses one page of memory at a time and gets it back when a program needs it.
That sounds like any compressor, but zram changes what "good" means. A program waits for every page
it gets back, so reading has to be fast on a page that is in no CPU cache. zram does not pay for the
bytes a page takes, it pays for slots of fixed sizes, so a few bytes saved often save nothing. The
codec runs in the kernel, once per CPU, so its own buffers are memory taken from every CPU for good,
and its decoder reads whatever is in RAM, also a page that was damaged.

What follows is zram seen from the codec's side: what swap is, the path a page takes into zram and
back, the allocator that stores it, the few functions zram calls in a codec, and the rules kernel
code lives by; then how the project scores a codec, which codecs zram has, and where seqlz stands
among them. None of it is seqlz's code yet, and the rest of the book refers back to it.

## Pages, swap, and a swap device in RAM {#ch-zram--swap}

A program sees its memory as one long range of addresses. The CPU and the kernel cut it into
**pages**, blocks of 4096 bytes on x86-64 and on most phones, and map each page of the program to a
page of RAM, or to none. A page is the smallest unit the kernel moves around: it gives memory to a
program a page at a time, and takes it away a page at a time.

When RAM runs short, the kernel **reclaims** pages: it looks for pages that were not used for a
while and frees them. An unchanged page of a file can simply be dropped, the file still has it. A page of a
program's heap or stack has nothing behind it, so before it can be freed its contents have to go
somewhere. That somewhere is **swap**, a block device, traditionally a partition on a disk. Writing a
page there is a swap-out. When the program touches the page again, the CPU finds no mapping, raises
a **page fault**, and the kernel reads the page back from swap into a fresh page of RAM, maps it, and
lets the program go on: a swap-in.

**zram** is a block device whose storage is RAM. Every page written to it is compressed and kept in
memory; reading it decompresses the page again. As swap, that trades CPU time for memory: a page
compressed to a quarter of its size frees three quarters of a page, and getting it back costs a few
microseconds instead of a disk access. Most Android phones and several Linux distributions, e.g. Fedora,
swap to zram. The project's numbers come from two such machines, a desktop and a phone
(a Xiaomi Mi 9T), and the rest of the book uses them as the two ends: a fast x86-64 core and a phone's
small arm64 core.

Every page is compressed on its own, 4096 bytes without the pages around it, so that any one page can
be read back alone. That is the first fact that shapes seqlz: a compressor has only 4096 bytes to find
repeats in and to learn from. A format that starts each input with a table of its own codes, as
`zstd` does, spends a good part of a page on the table (see [chapter 2](#ch-compression)).

## The path of a page {#ch-zram--path}

The figure shows the two paths with the names of zram's functions in mainline Linux, as of the kernel
the project checked against (`986c24e0fe44`, September 2026).

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

**Swap-out.** `zram_write_page()` first checks whether the page holds one value only, the same 8
bytes from start to end, e.g. a page of zeros. `page_same_filled()` compares the page as 512 words of
8 bytes; such a page is not compressed at all, zram keeps the 8 bytes in its table of slots and
nothing in the allocator. For every other page zram takes this CPU's **stream**, a struct with a
mutex, a buffer of 2 pages, and the codec's own work memory, and calls `zcomp_compress()`, which
calls the codec. The codec writes the compressed page into the buffer and returns its length. If the
length is 3625 bytes or more, zram stores the page as it is, 4096 bytes, see the next section.
Otherwise it asks the allocator, zsmalloc, for a slot of that size and copies the compressed bytes
there.

**Swap-in.** The program touches the page, the CPU faults, and the kernel's fault handler asks the
swap device for the page. zram looks up the slot: a same-filled page is filled with its value, a page
stored raw is copied, and only the rest reaches the codec. zram maps the slot, takes the stream again
and calls `zcomp_decompress()` with the compressed bytes and the page to write. When the codec
returns, the page is mapped into the program and the program goes on.

**Who waits for which.** A swap-out happens when the kernel reclaims memory. Most of it is done by
`kswapd`, a kernel thread per memory node that runs in the background when free memory drops below a
watermark. When memory runs out faster than `kswapd` frees it, a task that wants memory reclaims
itself, **direct reclaim**, and then that task waits for the compression too. A swap-in is always
done by the task that touched the page, inside its page fault: the program is stopped until the page
is back. So reading is on the critical path, and a write is on it sometimes.

**Cold.** A page that was swapped out was by definition not used for a while, and nobody read its
compressed bytes since they were written. They are in no CPU cache when the fault comes, they have
to come from RAM, about 100 ns per access on a desktop and more on a phone. The page the codec writes
into is cold too. A read in this state is a **cold read**, and it is the one that counts: a benchmark
that decodes the same page in a loop measures the decoder with everything in the cache, which no
swap-in ever sees. The project measures cold reads, with the compressed bytes flushed from the cache
first, and in the end the whole page fault in a virtual machine and on the phone
([`docs/plan.md` §3.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#32-decompression-is-a-tail-latency-problem-not-a-throughput-problem)).

::: measured How much of a swap-in is the codec
In a virtual machine on a Ryzen 9 7950X, with pages the project's desktop swapped into zram, a cold
swap-in with `lz4` takes 3.51 µs, of which 1.43 µs are `zcomp_decompress()`; with `seqlz-fast-lit` it
takes 4.13 µs, of which 2.04 µs are the codec. The kernel's part, the fault, the swap entry, zram's
slot, is about 1.9 µs for every codec. So a codec 40% slower than `lz4` makes a swap-in about 20%
slower
([explored-designs.md, "The whole page fault"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-whole-page-fault-the-kernels-part-is-the-same-for-every-codec-the-gap-to-lz4-about-halves)).
:::

## zsmalloc: bytes saved count only across a size class {#ch-zram--size-classes}

zram stores the compressed pages with **zsmalloc**, an allocator built for many small objects of
varied sizes. It does not hand out any number of bytes. It has **size classes**: nominal slot sizes in steps
of 16 bytes on 4 KiB pages (`ZS_SIZE_CLASS_DELTA`, the page size divided by 256), and a slot also
holds an 8-byte handle (`ZS_HANDLE_SIZE`). An object of $n$ bytes goes into the smallest class that
holds $n + 8$. Each class packs its slots into a "zspage" of 1 to 8 physical pages, chosen to waste as
little of the last page as possible, and classes that come out with the same packing are merged into
the larger one. The project ported this setup to C++ (`bench/zsmalloc_cost.cpp`) and checked it
against the kernel: after merging, the 255 nominal classes are 119 different ones
([`docs/plan.md` §3.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#31-zram-does-not-pay-for-bytes-it-pays-for-zsmalloc-size-classes)).

What a page costs is a step function of its compressed length. Around 1 KiB, after the merging, the
classes are 32 bytes apart:

| compressed length | slot | memory per page, with the zspage's waste |
| --- | --- | --- |
| 969 to 984 bytes | 992 | 993.0 |
| 985 to 1016 bytes | 1024 | 1024.0 |
| 1017 to 1048 bytes | 1056 | 1057.0 |

A page of 1017 bytes and one of 1048 bytes cost the same; saving 31 bytes on that page saves nothing,
and saving the one byte from 1017 to 1016 saves 33. The steps grow with the size: 17 bytes on
average below 1 KiB, 28 between 1 and 2 KiB, 68 between 2 and 3 KiB, and 93 from 3 KiB on, up to 144.
And at 3625 bytes there is a cliff. zsmalloc's largest classes hold one object per zspage, no better
than a whole page. zram asks zsmalloc where they start, `zs_huge_class_size()`, keeps the answer in
`huge_class_size`, and stores every page that compresses to that size, 3625 bytes, or more as it is,
4096 bytes.

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

- **Saving less than one class step on a page usually saves nothing.** A format can afford a few
  bytes of header per page, a flag per page, a field that makes the decoder faster. The rule
  that seqlz codes its literals only if that saves $\frac{1}{16}$ of them and {{ch01.LIT_CODED_MIN}} bytes more comes from this
  ([chapter 11](#ch-literals-enc)).
- **The cliff is the largest step.** A page moved from 3625 to 3600 bytes saves 455 bytes, 18 times
  the 25 bytes of compression it took. And zram never decompresses a page stored raw, so the decoder
  only ever sees pages below 3625 bytes.
- **The measure of size is not the mean ratio.** The project counts the **stored size**: the sum of
  what zsmalloc spends on the pages, "Σ zsmalloc cost", plus how many pages went over the cliff. On a
  new zram device that model equals zram's own `mem_used_total` to the byte
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zrams-memory-against-the-model-exact-on-a-new-device-σ-zsmalloc-cost-035-to-047-below-it)).

The project's metrics leave out pages that hold one value
([`docs/plan.md` §5.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#52-metrics)). zram
stores them without any codec, so they cost every codec the same, and counting them would make all
codecs look better without telling them apart. A few measurements quoted later, the swap times in
the kernel VM among them, include them; where that matters, the text says so.

## What zram calls: a backend's five functions {#ch-zram--zcomp}

Since 2024 zram has its own small interface for codecs, a **backend**, in `drivers/block/zram/zcomp.h`
(commit `917a59e81c34`). A backend is one struct of function pointers. This is the kernel's, not
part of `src/`:

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

- `setup_params()` runs once per zram device, when the device is set up with its algorithm. `struct
  zcomp_params` has the compression **level** and an optional **dictionary**, a block of bytes the
  user can load into the device, and a pointer `drv_data` for the backend's own data that all CPUs
  share. seqlz has two levels: 1 stores the literals of a page as they are (`seqlz-fast`), 2 codes
  them (`seqlz-fast-lit`, the default). It has no use for a dictionary: its tables are fixed and part
  of the format, and tables per device measured as not worth it, so the kernel backend refuses one
  with `-EOPNOTSUPP` ([chapter 15](#ch-zram-glue)).
- `create_ctx()` runs once per CPU. `struct zcomp_ctx` is a single `void *context`: whatever the
  codec needs as work memory, allocated here, never while a page is being compressed.
- `compress()` gets a `struct zcomp_req` with `src`, the page, `src_len`, always `PAGE_SIZE`,
  `dst`, the stream's buffer, and `dst_len`, its size, `2 * PAGE_SIZE`. It sets `dst_len` to the
  length it wrote and returns 0, or an error.
- `decompress()` gets the compressed bytes and their length, which zram stored, and the page to write,
  `dst_len = PAGE_SIZE`. The compressed page carries no length of its own and no checksum; the
  length zram passes in is all the decoder knows about where its input ends.

Each CPU's stream has a mutex, and zram holds it for the whole call, so one context is never used by
two calls at once. A series of Sergey Senozhatsky from 5th October 2026, not merged yet, splits the
stream into one for writing and one for reading, so that a reader does not wait for a writer that was
preempted while holding the mutex; a backend then has `create_cctx()` for compression and an optional
`create_dctx()` for decompression. The project's kernel backend is written for both forms
([`docs/plan.md` §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)).

The benchmarks of this repository call seqlz the same way, through a copy of this interface in C.
`src/zram_seqlz.c` is that glue, and its decompress function shows the whole contract in a few lines:

@listing src/zram_seqlz.c 124-135

It takes the compressed bytes `src` and their length `src_len` from the caller. First,
`quetschn_prefetch_page()` asks the CPU to start loading the cache lines (the 64-byte units the CPU
loads memory in, [chapter 3](#ch-cpu--cache)) of the compressed bytes and
of the output page at once, before the decoder needs them; it stands for what the project's
experiments let zram do before every decompression. Then the function refuses an output smaller than
a page, and calls `seqlz_decode()` with the tables in `p->drv_data`, which `setup_params()` built
once, and the context `s->context`, the scratch buffer this CPU decodes the literals into.
`seqlz_decode()` returns 0 for a valid page and a negative error code otherwise. The glue turns any
failure into `-1`, and on success sets `*dst_len` to the page's size, which is how the interface
reports how many bytes were written. [Chapter 15](#ch-zram-glue) goes through the file.

## Work memory is per CPU, and counted as allocated {#ch-zram--work-memory}

::: kernel Per-CPU data, kmalloc() and vmalloc()
The kernel keeps some data once per CPU, "per-CPU data", so that code running on one CPU can use its
copy without a lock against the others. zram's streams are such data: a machine with 8 CPUs has 8
streams, each with its own buffer and its own codec context, all allocated when the device is set up
and kept as long as it exists, whether that CPU ever swaps or not.

The kernel has two main allocators. `kmalloc()` hands out physically contiguous memory from caches of
fixed sizes, powers of 2 up to `KMALLOC_MAX_CACHE_SIZE`, which is 2 pages, 8 KiB, with 4 KiB pages: a
request of {{const.SCRATCH}} bytes gets an object of {{ch01.dctx_kmalloc}}. A larger request gets whole pages straight from the page
allocator, again a power of 2 of them: 16 384 bytes are 4 pages, 61 440 bytes get 16 pages, 65 536.
`vmalloc()` maps whole pages into a contiguous range of addresses, so it rounds up to whole pages: a
request of 16 416 bytes takes 5 pages, 20 480 bytes. `kvmalloc()` tries the first and falls back to the
second.
:::

zram already allocates 3 pages per CPU for every codec: the buffer of 2 pages that compression
writes into, and a page `local_copy` for reading an object that zsmalloc split over two physical
pages. The codec's context comes on top. For the existing codecs it is:

| codec | work memory asked for, per CPU | what the allocator hands out |
| --- | --- | --- |
| `lzo`, `lzo-rle` | 16 384 bytes, `kzalloc()` | 16 384 bytes |
| `lz4` | 16 416 bytes, `vmalloc()` | 20 480 bytes, 5 pages |
| `842` | 61 440 bytes, `kmalloc()` | 65 536 bytes, 16 pages |
| `zstd` 3 | 186 112 bytes, `vzalloc()`, for compression and decompression | 192 544 bytes, 47 pages and a struct |

The plan calls this memory **C5**, and it is a cost: on the project's test phone, with 8 cores, every
4 KiB of work memory per CPU is 32 KiB of RAM that is never free, on the device where RAM is shortest.
Until October 2026 it was a limit, at most `lz4`'s 16 416 bytes. Now the score counts it, spread over
the pages in zram: with 1 GiB in zram those 32 KiB are 0.1 bytes per page, with 64 MiB 2 bytes
([`docs/plan.md` §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)).
seqlz needs a table of {{size.state}} bytes for the matcher while it compresses and a buffer of
{{const.SCRATCH}} bytes for the decoded literals while it decompresses. zram holds the stream's mutex for either, and neither
keeps anything from one page to the next, so the kernel backend puts both into one union of
{{ch01.union}} bytes, which `kmalloc()` hands out as exactly {{ch01.union_kib}} KiB. With the split streams they
are two allocations, {{ch01.cctx_kib}} KiB each after rounding, {{ch01.split_kib}} KiB per CPU, below `lz4`'s
20 KiB. The tables seqlz decodes with, {{size.tables|grp}} bytes, are not per CPU: they are built once
when the kernel starts and shared by all CPUs.
@assert ch01.union == 1024 * ch01.union_kib and ch01.cctx_kib == ch01.dctx_kib and ch01.split_kib < 20
[Chapter 6](#ch-header) and [chapter 15](#ch-zram-glue) have the details, and why 16 KiB pages break
the limit.

## The decoder must survive any input {#ch-zram--safety}

zram stores no checksum. What the decoder gets is whatever is in the slot when the page is read, and
that is not always what the compressor wrote: a bit can flip in RAM, a bug elsewhere in the kernel can
write over memory zsmalloc owns, and with zram's writeback the page went to a disk and came back. The
decoder runs in the kernel, so a page that makes it read past its input or write past the output page
does not crash one program, it corrupts the kernel's memory or reads someone else's.

So the project's second hard limit, **C6**, is that the decompressor is safe for any input and bounded
in time: whatever bytes it gets, it reads only its input and its tables, writes only the output page
and its scratch, and returns within a fixed amount of work, with an error for a page that is not
valid. zram then prints a warning and fails the read; one page is lost, the kernel is not.

The reason C6 is a hard limit is history. The last new codec proposed for zram, zBeWalgo in 2018,
reached version 7 of its patch series and was never merged. One of the objections on the mailing list
was exactly this, in Eric Biggers's words: *"You cannot just ignore fuzz-safety in the decompressor
either"*
([`docs/plan.md` §2.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#22-the-failure-zbewalgo-benjamin-warnke-2018-reached-v7-never-merged)).
seqlz's decoder checks every length and offset against the room left in the page and in the literals
before it uses them, stops after at most `PAGE / 4 + 1` sequences, and requires that a page ends exactly
where its bits end. Fuzzers, programs that feed a function random and damaged inputs and report
every crash and every read out of bounds ([chapter 15](#ch-zram-glue--fuzzing)), fed it billions of
pages. Of the pages the project built to be slow, the slowest decodes in 1.33 times the p99 time of
real pages, measured on the Mi 9T's small core, a Cortex-A55, where `lz4`'s slowest takes 1.57 times
its own. Counted in instructions, the costliest page a search for slow pages found decodes in 1.59
times seqlz's p99, and the same search found a page with 2.58 times `lz4`'s p99 for `lz4`
([explored-designs.md, "The worst case"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-worst-case-the-slowest-pages-found-cost-13-times-the-p99-of-real-ones-as-for-lz4)).
[Chapter 14](#ch-decode) shows each check where it is.

## What kernel code cannot do {#ch-zram--kernel-rules}

A codec in the kernel is C, but not the C of a normal program:

- **No C library.** There is no `malloc()`, no `printf()`, no `<string.h>` from libc; the kernel has
  its own `memcpy()` and `memset()`, and its own names for fixed-size types, `u8` to `u64`. seqlz
  allocates nothing at all: zram passes in the work memory.
- **No floating point and no vector registers.** When the kernel is entered, it does not save the
  program's floating point and SIMD registers, so kernel code must not touch them, and the kernel is
  compiled so that the compiler does not use them on its own (`-mno-sse` and friends on x86-64,
  `-mgeneral-regs-only` on arm64). A piece of code can borrow them with `kernel_fpu_begin()` or
  `kernel_neon_begin()`, but that saves and restores state, costs time and is not allowed in every
  context. Every compressor in the kernel is scalar code on the integer registers, and so is seqlz.
  A faster SIMD decoder is not an option, however much it would gain.
- **A small stack.** A kernel thread has a stack of 16 KiB on x86-64 and arm64, and a function's
  frame should stay small. The build warns about a frame above `CONFIG_FRAME_WARN`, 2048 bytes on
  64-bit and 1280 on most 32-bit builds in the kernel of September 2026 (1024 in older ones). The
  project keeps every frame of the kernel copy below 1024 bytes; the deepest are 640 bytes, in
  `code_literals()` and `decode_literals()` built with gcc for x86-64
  ([chapter 15](#ch-zram-glue--check)). No recursion, no arrays sized at run time.
- **No sleeping in compress and decompress.** zram has called the codec with preemption disabled,
  older kernels such as the phone's 4.14 still do, and a fix proposed in August 2026 would bring it
  back for backends like seqlz. So the codec must not wait for anything: no allocation, no lock, no
  page fault
  ([`docs/plan.md` §3.2](https://github.com/martinus/quetschn/blob/main/docs/plan.md#32-decompression-is-a-tail-latency-problem-not-a-throughput-problem)).
  All of that happens in `create_ctx()`.
- **Every architecture.** The same source runs on little-endian x86-64 and arm64 and on big-endian
  s390, and on 32-bit arm. seqlz reads multi-byte values with the kernel's `get_unaligned_le64()` and
  friends, never by casting a pointer, and CI checks that x86-64, 32-bit x86 and s390x write the same compressed bytes for the same
pages.
- **No inline assembly, nothing that depends on the CPU model.** A maintainer of `lib/` reads one C
  source for all machines. A gap in what the kernel offers is fixed in the kernel, with a patch of its
  own; [chapter 5](#ch-compat) shows the one seqlz needs.

`src/` follows all of this, and `src/seqlz_compat.h` supplies the kernel's names outside the kernel,
so that the benchmarks, the tests and the fuzzers build the same code
([chapter 5](#ch-compat)).

## The score: bytes against time {#ch-zram--score}

A codec is good for zram if it stores pages small and handles them fast, and no codec is best on both.
The project's first plan had bars: so much smaller than `lz4`, a p99 time no worse than `lz4`'s. Bars
turned out to be cliffs for design choices: a change that saved 4% of memory failed because its p99
was 1 µs over the bar, without asking what 1 µs is worth. So designs are judged by a **score**
([`docs/plan.md` §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)):

- **bytes**: the memory zsmalloc spends per stored page, the stored size of the section above.
- **time**: per page written to zram, $\text{write} + r \times \text{read}$, each the **mean** over all pages, in µs.
- **score**: $\text{bytes} + \lambda \times \text{time}$, lower is better.

**Why the mean.** The mean is the total time spent on the pages. When a program that was idle comes
back, it faults in many pages one after the other, and it waits for the sum. The 99th percentile, p99,
the time 99% of the pages stay below, stays a guard: a p99 that gets clearly worse means some pages
are pathological, and they are looked for. It is not a target.

**Why $r = 0.34$.** Every page written is read back at most once, often never: it is freed, or the
program exits. $r$ is how many reads come per write. On the project's desktop with zram as its only
swap, the kernel's counters `pswpin` and `pswpout` in `/proc/vmstat` were 3 146 179 pages swapped in
against 9 210 320 swapped out in 27.5 days: 0.34. So the time of a write counts fully and the time of
a read a third. That is one machine; a phone swaps differently, and its $r$ is not measured yet.

**lambda** is an exchange rate, in bytes per µs: how many bytes of stored size one µs per page is
worth to you. It is not fixed. On a phone short of RAM a byte is worth more than on a server with
memory to spare. So the project does not pick one. It plots each codec at (time, bytes) and draws the
lower left convex hull: the codecs that have the lowest score for some lambda. Between two neighbours
on the hull, the slope is the exchange rate at which the faster one starts to win. A design is worth
keeping if it is on the hull at an exchange rate that matters. `quetschn-score` computes the hull from
the benchmark logs.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 360" role="img" aria-label="Stored bytes per page against time per page written for lz4, lzo-rle, seqlz-fast-lit and zstd 3, means over the pages of a desktop's zram, with the lower left hull from lzo-rle through seqlz-fast-lit to zstd 3.">
  <g fill="none" stroke="currentColor" stroke-width="1">
    <line x1="70" y1="320" x2="690" y2="320"/><line x1="70" y1="320" x2="70" y2="30"/>
  </g>
  <g class="mut" fill="none" stroke="currentColor" stroke-width="0.6">
    <line x1="215.9" y1="320" x2="215.9" y2="30"/><line x1="361.8" y1="320" x2="361.8" y2="30"/>
    <line x1="507.6" y1="320" x2="507.6" y2="30"/><line x1="653.5" y1="320" x2="653.5" y2="30"/>
    <line x1="70" y1="247.5" x2="690" y2="247.5"/><line x1="70" y1="175" x2="690" y2="175"/>
    <line x1="70" y1="102.5" x2="690" y2="102.5"/><line x1="70" y1="30" x2="690" y2="30"/>
  </g>
  <g font-size="11">
    <text x="70" y="336" text-anchor="middle">0</text><text x="215.9" y="336" text-anchor="middle">4</text>
    <text x="361.8" y="336" text-anchor="middle">8</text><text x="507.6" y="336" text-anchor="middle">12</text>
    <text x="653.5" y="336" text-anchor="middle">16</text>
    <text x="64" y="324" text-anchor="end">0</text><text x="64" y="251.5" text-anchor="end">500</text>
    <text x="64" y="179" text-anchor="end">1000</text><text x="64" y="106.5" text-anchor="end">1500</text>
    <text x="64" y="34" text-anchor="end">2000</text>
    <text x="380" y="354" text-anchor="middle">time per page written, write + 0.34 × cold read, µs</text>
    <text x="70" y="20">stored bytes per page</text>
  </g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="2" stroke-dasharray="6 4">
    <path d="M301.4,99.6 L352.1,148.5 L645.7,159.8"/>
  </g>
  <g fill="currentColor">
    <circle cx="302.5" cy="87.6" r="4.5"/>
    <circle cx="301.4" cy="99.6" r="4.5"/>
    <circle cx="645.7" cy="159.8" r="4.5"/>
  </g>
  <g class="acc"><circle cx="352.1" cy="148.5" r="5.5" fill="currentColor"/></g>
  <g font-size="11" class="halo">
    <text x="311" y="84">lz4, 6.38 µs, 1602 B</text>
    <text x="293" y="117" text-anchor="end">lzo-rle, 6.35 µs, 1520 B</text>
    <text x="360" y="170">seqlz-fast-lit, 7.74 µs, 1183 B</text>
    <text x="690" y="146" text-anchor="end">zstd 3, 15.79 µs, 1105 B</text>
    <text x="334" y="118" class="acc">243 B/µs</text>
    <text x="440" y="148" class="acc">9.7 B/µs</text>
  </g>
</svg>
</div>
<figcaption>The four codecs of the table below, means over the pages; the dashed line is the hull, and the slopes are the exchange rates between its neighbours.</figcaption>
</figure>

Read the figure from the right; its numbers are the ones of the table in the next section, measured
on 5th October 2026. Going from `zstd` 3 to `seqlz-fast-lit` saves 8.05 µs per page written
and costs 78 bytes per page, 9.7 bytes per µs: whoever values a µs at less than 9.7 bytes should run
`zstd`. Going from `seqlz-fast-lit` to `lzo-rle` saves 1.39 µs and costs 337 bytes, 243 bytes per µs:
whoever values a µs at more than that should run `lzo-rle`. In between, `seqlz-fast-lit` has the
lowest score. `lz4` is not on the hull: `lzo-rle` is faster and smaller in these runs.

With the times of a real swap-out and swap-in in the virtual machine, which the project measured
later and which are the numbers its plan quotes, `seqlz-fast-lit` has the lowest score for any lambda
from 6.8 to 190 bytes per µs; the hull is `lz4`, `lzo-rle`, `seqlz-fast`, `seqlz-fast-lit` and
`zstd` 3
([explored-designs.md, "The score with the swap times"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-score-with-the-swap-times-seqlz-fast-lits-range-ends-at-155-and-211-bytes-per-µs-instead-of-200-and-279)).
Whoever runs `lz4` or `lzo-rle` today instead of `zstd` says, by that choice, that a µs is worth more
than about 7 bytes to them.

## The codecs zram has, and where seqlz sits {#ch-zram--codecs}

Mainline zram has seven backends: `lzo-rle`, the default, `lzo`, `lz4`, `lz4hc`, `zstd`, `deflate`
and `842`. Three matter for the comparison. `lz4` is the fast one, the common choice on Android.
`lzo-rle` is zram's default, an `lzo` with a shortcut for runs of zeros, merged in 2019. `zstd` stores
pages smallest and takes about twice the time. `lz4hc` writes `lz4`'s format with a slower, more
thorough search. `deflate` and `842` are not in the comparisons of this book.

All of them, and seqlz, work the same way at the core: they cut a page into literals and matches,
"copy so many bytes from so far back", and they differ in how hard they search and how they write the
result down, in whole bytes like `lz4` and `lzo`, or in bits with codes like `zstd` and seqlz.
[Chapter 2](#ch-compression) explains both halves from zero.

This is where seqlz sits, measured in the real kernel in a virtual machine
(`tools/zram-vm/run.sh`), on one CPU of a Ryzen 9 7950X fixed at 4.5 GHz, on 40 000 pages the
project's desktop swapped into zram in September 2026. A program in the VM writes
each page to zram with `pwrite()` and reads it back with `pread()`; each time is the whole system call,
zram and zsmalloc included, the median of 3 runs, and for a cold read the compressed page is flushed from
the CPU caches first. Means over the pages, measured on 5th October 2026
with the tables seqlz had before 7th October
([`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-result-lz4s-time-zstds-size-almost)):

| codec | stored bytes per page | write, µs | cold read, µs | time per page written, µs |
| --- | --- | --- | --- | --- |
| `lz4` | 1602 | 5.52 | 2.54 | 6.38 |
| `lzo-rle` | 1520 | 5.40 | 2.78 | 6.35 |
| **`seqlz-fast-lit`** | **1183** | **6.79** | **2.79** | **7.74** |
| `zstd` 3 | 1105 | 13.95 | 5.41 | 15.79 |

The time per page written is the write plus 0.34 times the cold read, e.g. for `lz4`
$5.52 + 0.34 \times 2.54 = 6.38$ µs. In these runs of 5th October 2026, `seqlz-fast-lit` stores
pages 22% smaller than `lzo-rle`, 26% smaller than `lz4`, and within 6% of
`zstd` 3, for 23% more time than `lz4` to write a page.

::: warn The read times in this table flatter the decoder
The project's read benchmark in the VM reads every page several times in a row in each run, warm and
cold, and that trains the CPU's branch predictor on the page: the predictor guesses the outcome of
each `if` from its past ([chapter 3](#ch-cpu--branches)). In a real swap-in, which decodes a page once, `seqlz-fast-lit` mispredicts about 110 branches
per page more, and its reads are 0.62 µs slower than `lz4`'s instead of the 0.25 µs
above
([explored-designs.md, "The decoder in a fault, found"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
The hull of the previous section uses the swap times. On the phone, in its own Linux 4.14 kernel,
`seqlz-fast-lit` stores pages 28% smaller than `lz4` and swaps them in 11% to 13% slower warm and 17%
to 25% slower cold.
:::

## The goal, and the limits it lives within {#ch-zram--goal}

The goal is a codec merged into mainline Linux as a zram backend because it is measurably better than
what is there. Better than everything on every axis is not possible: no small scalar codec without
allocation beats `zstd`'s entropy coder ([chapter 2](#ch-compression--entropy)) on size. So the target is the fast corner: as close as possible
to `lz4`'s speed and to `zstd`'s size. It
might be unreachable; a design is judged by how much closer it gets
([`docs/plan.md` §1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria)).

The plan writes the merge argument as six criteria. Their status on 9th October 2026:

| | criterion | status, as in [`docs/plan.md` §1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria) |
| --- | --- | --- |
| C1 | at least 8% less Σ zsmalloc cost than the better of `lzo-rle` and `lz4` | met: 22% less than `lzo-rle` on the desktop, 28% less than `lz4` on the phone |
| C2 | the p99 cold read time against `lz4`, on x86-64 and on an arm64 little core | slower |
| C3 | the p99 write time against `lz4` | 1.20 times |
| C4 | beats `lz4` with a trained dictionary too | met: 20.4% less |
| C5 | work memory per CPU | 8 KiB with 4 KiB pages, 16 KiB with the contexts split |
| C6 | the decoder safe for any input and bounded in time | met |

C6 is the only hard limit: an unsafe decoder is not a trade-off. C5 is a cost, counted in the score's
bytes for a named machine, e.g. 8 CPUs and 1 GiB in zram. C1 and C4 are reported. C2 and C3 are guards,
not gates. A design is never dropped because
its p99 misses the bar, and never kept because it meets it; the score decides. Since C2 did not hold,
the argument for a merge is memory at a small cost in time, not a faster read.

**No recompression.** zram can compress idle pages a second time, with a slower algorithm, after they
were written. The project measured that, with an optimal parser for seqlz's format (one that searches
for the cheapest sequences of a page, not the first match) and with `zstd` 3, 16 times as fast as
that, and did not pursue it, see [chapter 17](#ch-decisions--not-kept).
So every design in this book compresses a page once, when it is written, and that one pass is all the
time it gets.

Two numbers in this chapter rest on one machine each: $r = 0.34$ is the desktop's, and a phone's $r$
is not measured; and how large the bursts of swap-ins are, which decides how much a slower read hurts
a user, is not measured either. With 16 KiB pages, which Android is moving to and which the project
has only from the Android 17 emulator, not from a phone, seqlz needs more work
memory than `lz4` once the streams are split, which the score counts as a cost.
@assert k16.size.state + k16.const.SCRATCH > 16416 and k16.size.state <= 16416
