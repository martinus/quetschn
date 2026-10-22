# How to write a chapter of "seqlz, line by line"

The book explains every line of `src/` of this repository to a developer who knows C and C++ well, but has never written kernel code and never written a compressor. It
is a single HTML page, built by `build.py` from `chapters/*.md`. Think O'Reilly book: the original code
in sections, each section explained, so that after reading the reader understands every line, the
structure and architecture, and why each choice was taken.

## Depth: what "explained" means

For every listing, the prose after it covers:

1. **What** the code does that a C programmer does not see at a glance. Do not paraphrase lines that
   read plainly (a loop, an `if`, a `memcpy()`, a `static inline` helper, a cast); say what a group of
   lines achieves, then spend the words on the lines that need them. A comment is explained only when
   there is more to it than it says.
2. **How** it works where that is not standard C knowledge: kernel idioms, the bit layout of a value,
   why a branch-free form, what the CPU does with it. Do not explain the language: integer promotion,
   unsigned wrap-around, shifts and masks, `static inline`, casts, `static_assert` and undefined
   behaviour are known to the reader; mention them only where the code depends on a rule in a way
   that is easy to miss, in one sentence. A bit trick that is not obvious gets a **worked example
   with real numbers**, e.g. "for `off = 24`, `(off & 7) == 0` is 1, so ...". Compute such numbers, do
   not guess them: use `python3`, or compile a few lines against `src/` with `gcc -O2 -Isrc` (in your
   scratch dir).
3. **Why** it is written this way and not the obvious other way: the design reason, and where the
   repository measured it. Reasons come from the code's comments, `docs/seqlz.md`, `docs/format.md`,
   `docs/plan.md`, `docs/explored-designs.md` and `git log`. Never invent a measurement. When you quote
   a number, say what it was measured on (which CPU, which pages, kernel VM or userspace) and link the
   source.
   One number per measurement: where the source has one value per dump, quote their mean, and do not
   name or number the dumps ("on the first desktop dump", "24 and 2 bytes"); the reader should not
   have to know how many dumps there were. The per-dump values stay in `explored-designs.md`, which
   the link leads to.
4. **Where it connects**: which other function uses this, which chapter explains that.

The reader knows C and C++: the language, its integer rules and undefined behaviour, the standard
library, what a compiler inlines and how a struct is laid out. The reader does not know: what zram, zsmalloc, a page,
`kmalloc()`, per-CPU data, KUnit, `EXPORT_SYMBOL` or `__init` are; what LZ77, Huffman, canonical codes,
prefix codes, entropy are; what a cache line, a branch misprediction, an in-order core, a dependency
chain, SWAR, a prefetch are. Explain each the first time your chapter needs it, briefly, in a `:::
kernel` or `::: cpu` box when it is more than two sentences. Part I explains the basics once; Part II
chapters may link back to them instead of repeating, e.g. `[canonical codes](#ch-compression--canonical-codes)`,
but a one-sentence reminder is fine.

Length: as short as the code allows. A chapter of 150 lines of code is typically 1500 to 3000 words.
Say a thing once: no summary of what a listing showed, no repeat of what another chapter explains
(link it).

## Listings

```text
@listing src/page_lz.h 13-22
```

on a line of its own inserts lines 13 to 22 of that file, with real line numbers, taken from the
repository at build time. Never paste code from `src/` by hand; always use `@listing`.

- Your chapter has an assigned set of line ranges. **Every non-blank line in them must be in some
  listing of your chapter.** `python3 build.py` prints `not covered:` lines (for all chapters; look
  at your files). Small overlaps are fine.
- Cut listings at natural boundaries: a function, a group of related `#define`s, a loop, a block of a
  long function. 3 to 40 lines each; a 100-line function becomes several listings with prose between.
- Mostly in file order. Going out of order is fine when the explanation needs it (e.g. a helper
  before its caller), say so.
- For short code that is not from `src/` (an example, a simplified version, pseudo-code, a hex dump),
  use a fenced block: ` ```c `, ` ```text `, ` ```python `.

## Boxes

```text
::: why Title, optional, inline markdown allowed
Markdown content, may contain lists, fenced code.
:::
```

Kinds: `why` (a design decision and what it was measured against), `kernel` (kernel background),
`cpu` (CPU background), `measured` (a measurement with its setup), `warn` (a trap, a subtle point
that is easy to get wrong), `try` (something the reader can run to see it: a command, a few lines of
C or Python). Use them where they help, not on every page; most explanation is plain prose.

## Diagrams

Draw a diagram when it shows a mechanism prose cannot: the layout of a compressed page, the bit fields
of a table entry, the encoder writing literals from the front and bits from the back of one buffer, a
bitstream being consumed, 8 streams side by side. Inline SVG, in this exact wrapper, with a blank line
before and after:

```html
<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 200" role="img" aria-label="what the figure shows, one sentence">
  ...
</svg>
</div>
<figcaption>What the figure shows, one sentence.</figcaption>
</figure>
```

- Strokes and text use `currentColor` (`stroke="currentColor"`, text inherits `fill: currentColor`).
  For the one thing that matters, use `class="acc"` (accent colour) or `class="mark"` (second colour)
  on a group or element and draw with `currentColor` inside it. Fills: `class="fill-sheet"` (light
  box), `class="fill-paper"`, `class="fill-acc"`, `class="fill-mark"`; every shape gets an explicit
  fill (`fill="none"` or a class). No literal colours.
- Text 11 to 13 px at the drawn scale, monospace is the default font. Width up to 720 units.
- No `<script>`, `<style>` or `<foreignObject>` inside the SVG. Ids inside a figure must be unique in
  the whole book: prefix them with your chapter id, e.g. `id="ch-matcher-arrow"`.
- Mermaid also works: `<pre class="mermaid">flowchart LR ...</pre>`, for simple flowcharts.
- Check that every number and label in a diagram is right (bit positions, byte offsets).

## Structure and cross-references

- First line: `# Title {#ch-id}` with your chapter's id (given in your task). The title is a claim or
  a plain name, not a pun.
- `## Section {#ch-id--short}`: give each h2 an explicit id starting with your chapter id and `--`.
  h3 get ids automatically (`ch-id--slug-of-heading`).
- Link to other chapters by their ids: `#preface`, `#ch-zram`, `#ch-compression`, `#ch-cpu`,
  `#ch-format`, `#ch-compat`, `#ch-header`, `#ch-internal`, `#ch-codes`, `#ch-matcher`, `#ch-encoder`,
  `#ch-literals-enc`, `#ch-dec-basics`, `#ch-literals-dec`, `#ch-decode`, `#ch-zram-glue`,
  `#ch-trace`, `#ch-decisions`, `#glossary`. Links to h2 sections of other chapters only if you know
  the id; else link the chapter.
- Link to listings of other chapters by function name in prose ("`seqlz_br_refill()` in
  [chapter 12](#ch-dec-basics)"); the ids of listings are not stable.
- Repository files and docs: link to GitHub,
  `https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#<anchor>`. The anchor is
  GitHub's slug of the heading: lowercase, every character that is not a letter, digit, space,
  hyphen or underscore removed, spaces to hyphens (`µ` and `é` stay). Copy the heading exactly from the
  file and derive the slug; e.g. "The matcher without its step: writes 1% to 3% faster, kept" becomes
  `the-matcher-without-its-step-writes-1-to-3-faster-kept`. Look up the full heading with
  `grep -n '^#' docs/explored-designs.md`.
- No "In this chapter we will ..." opener and no "In this chapter we learned ..." closer. Start with
  the substance: one or two paragraphs that say what the code does and how it fits, then the code.
- End a chapter with the limitation or open point, if there is one, or simply with the last
  explanation.

## Voice

Martin's voice, blog-post register. These rules are hard:

- No em dashes and no en dashes in prose. Use a comma, a colon, a full stop, or rarely a hyphen with
  spaces. Ranges in words: "3 to 40", not "3-40".
- `e.g.`, never "for example" or "for instance". Lowercase, two dots; at a sentence start "E.g.".
- Plain words, a small vocabulary. Short sentences mixed with longer ones, about 18 to 22 words on
  average. Active voice.
- No hype and no claim without an axis: not "blazing fast", not "elegant", not "clever"; "5% fewer
  cycles to compress a page on a Ryzen 9 7950X in userspace" is the style. Put the caveat last.
- Banned words: delve, leverage, meticulously, seamless, nuanced, myriad, testament, load-bearing,
  utilize, facilitate, robust (rarely), crucial (rarely), "it's worth noting", "not just X but Y",
  "Let's", "dive", "journey", "under the hood", "magic".
- No judgment of other projects' code; report the numbers.
- Backticks on every code token: function names with `()`, `SEQLZ_BLOCK_SIZE`, file names, numbers in code
  form when they are code (`0x8000`).
- Numbers as digits. Dates as "8th October 2026". Units: "8 KiB", "4096 bytes", "2.7 µs", "11 bits".
- Headings state the point when they can: "The hash keeps 5 bytes, not 4" beats "The hash".
- "we" for the reader and the book walking through the code together, "you" for the reader. The
  author of the code is "the code" or "seqlz", or "the project"; do not invent quotes from people.
- Use the terms of the book's glossary, `chapters/18-glossary.md`, consistently: page, literal, match,
  offset, repeat offset, `ll`, `ml`, sequence, token, offset class, offset bits, length value, escape,
  symbol, code, code length, prefix code, Huffman code, complete, table, literal table, bitstream,
  header, coded literals, stream, stored size, work memory, size class, matcher, encoder, decoder.

## Facts and secrets

- Facts about the code come from the code. Check them by reading and, where cheap, by running.
- A value the code computes is never typed: bytes, bit strings, codes, counts, sizes, what a program
  prints. Quote it as `{{key}}` from a program of `facts/`, show a program's output with `@program`
  and `@output`, and put an `@assert` next to a sentence that depends on the code without a number in
  it (README.md has the syntax). A measured number is typed, with a date or a link in its paragraph.
- Never include contents of corpus files (`corpus/`, `*.pages`): they hold keys and passwords. Pages
  you construct yourself (zeros, `ab` repeated, an array of pointers you make up) are fine.
- Do not edit anything outside `docs/book/`. Test programs go into a scratch directory, not the repository.
- Build with `python3 build.py` in the book directory to see your chapter render and to check
  coverage and links (`warning: link to missing id`). Other chapters may be missing while you work;
  their placeholders are fine.
