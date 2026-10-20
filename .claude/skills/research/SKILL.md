---
name: research
description: Researches seqlz instead of engineering it. Finds where the stored bits and the decode cycles still go, what any method could reach there, and which mechanisms could get closer, before any design is proposed. Use when Martin asks for research, new ideas, substantial (not percent) gains, "start from scratch", "where are the bits", a bound or a ceiling, or a research question about the codec or its data.
argument-hint: "[optional: a research question, e.g. \"how much of a page is pointers\"]"
---

# Research

Engineering asks "is X better than Y". Research asks "where is the rest, and what explains it". On
17 to 19 Oct 2026 a session asked for ideas and got variations of `docs/explored-designs.md`: every
idea came from memory, none from the data. This skill is the order that avoids that.

**The result is knowledge, not a merged design**: bounds, explained gaps, surprises, falsified
hypotheses, and directions ranked by evidence. A design only comes at the end, and only for a gap
that a measurement showed.

CLAUDE.md stays the rule book (corpus never committed or pasted, no benchmark while another runs,
Martin's voice for public text). This file changes only the order of the work and what counts as
done.

## Rules that differ from engineering

- **Bounds before designs.** No design is proposed before the bit accounting (step 2) says where it
  would act and how much is there.
- **A prediction before every run.** Write the expected number and the reason into the log first. A
  result outside the predicted range is a surprise (step 6), whatever it means for the codec.
- **Generate before reading the past.** Step 3 runs before `docs/explored-designs.md` is read, so
  that the old designs do not set the search space. Step 4 then checks every idea against it; nothing
  is proposed to Martin before that check (CLAUDE.md, Measuring).
- **Constraints off, then on.** Steps 2 and 3 ignore the kernel's rules (pure C, work memory, one
  format, upstream review, no zram change). Step 7 puts them back and says what survives and at
  what cost. Without this the search stays next to what exists.
- **Sizes and ideal code lengths first, time last.** A size or a bit count is exact and costs
  seconds; a timing needs a quiet machine and a phone. Time a candidate only when its size result
  survived.
- **Time a decoder the way zram calls it.** A userspace loop over a sample streams megabytes of
  compressed pages; prefetch each page's compressed bytes first, as zram's glue does
  (`quetschn_prefetch_page()`), and use the kernel's compile flags. On 19 Oct a loop without the
  prefetch had seqlz-1 at 11.5 µs per page on the A55 instead of 7.35, and two rounds of research
  chased a read gap that zram does not have.
- **Compare against the hull, not a neighbour.** A new point is judged against the line between
  its two neighbours on the current hull (`quetschn-score`), not against one design. On 19 Oct a
  per-page step choice was estimated 25 bytes per page below the line from step 4 to step 1, and
  measured 6 to 44 bytes above the line from step 4 to step 2.
- **Kill fast.** An estimate that cannot reach a quarter of the gap it aims at ends the idea; write
  one line why and move on.

## Order

### 1. The question

One sentence, measurable, with the data it is measured on. From Martin's argument, else from the
largest unexplained gap of the last report, else from **Starting questions** below. Write it at the
top of the log.

The log: `~/quetschn-corpus/research/<yyyy-mm-dd>-<topic>.md`, outside the repository, so it
survives the session and is never published. It holds predictions, results, surprises, killed ideas,
commands. Never page contents, only counts and sizes.

### 2. Account for the bits, then bound each part

Read `known-bounds.md` in this skill's directory first: what earlier sessions measured, with dates.

On a sample (the ten 20 000-page samples of CLAUDE.md, Measuring; for file data
`~/quetschn-corpus/erofs-exp/`), per page and per level:

1. **Where the stored bytes go**: header, token codes, offset bits, length values, literals (coded and
   raw), pages stored raw. `seqlz_find()` plus the tables' code lengths, or `tools/seqlz-bound/`
   (`offsets.py`, `space.py` count token and offset bits).
2. **A ladder of references** on the same pages: `lz4`, seqlz levels, `zstd` 3 and 19, `xz -9e` per
   page (the per-page ceiling of practical codecs), and the same with a window over all pages
   (`zstd --long=27 -19` on the concatenated sample: the ceiling with shared state).
3. **A bound per part, with ideal code lengths** (-log2 p, no decoder needed): for the literals
   order 0 per page, order 1, by byte lane (offset mod 4, 8), by the byte at a fixed distance; for
   the offsets the same, plus offsets that equal object sizes or repeat across pages; for whole
   pages exact and near copies. Each model is a few lines of numpy.
4. **The gap table**: part, bytes per page now, bound, gap, which model closes it. This table decides
   what to think about. The same for decode cycles when the question is speed: instructions per
   page by part (`simpleperf` on the Mi 9T, `perf` on the PC) against the instructions the parse
   cannot avoid.

### 3. Diverge

For the two or three largest gaps, at least 20 hypotheses about the mechanism behind them, absurd
ones included, constraints off. Each with **a testable consequence**: what has to be true in the data
if it holds ("if pointers dominate the offsets, the high 32 bits of the matched words repeat within
a page"). No filtering in this step, no reading of explored-designs.md. With several independent
gaps, one subagent per gap (Agent tool) in parallel, each with this step and step 5.

### 4. Literature and novelty

Per cluster of hypotheses one narrow web search (WebSearch, or the deep-research skill for a
survey): memory compression in hardware (BDI, FPC, BPC and newer), pointer and heap compression,
entropy decoding on in-order cores without SIMD, structured-data LZ, recent zram/zswap work. Then
the Catalog of `docs/explored-designs.md` (`grep -n -i` on the terms and the old names). Mark each
hypothesis: **new**, **known elsewhere** (with the link), **tried here** (with the entry and why it
failed: a reason can expire, e.g. a choice made at r = 0.34 before the phone's 0.73).

### 5. Cheap tests

The five hypotheses with the largest gap times plausibility, each an offline estimate of at most an
hour: an ideal-cost model, an oracle (the best choice per page, known in hindsight), an upper bound.
Prediction first. A tool goes into the scratchpad, reads pages, writes only numbers. Python with
`-I` on corpus data. Long jobs per CLAUDE.md (setsid, kill by process group).

### 6. Surprises first

Every result outside its prediction gets a line in the log and one follow-up question. A surprise
outranks the next planned test. Examples of surprises that were worth a question: on 19 Oct a hash
slot of an earlier page treated as empty compressed page 0 better than a cleared one; file blocks had
three times the literals of swap pages; references chosen in one boot still saved half of their gain
in the next; 32% of the pages of 4 Oct were copies of a page of 3 Oct.

### 7. Constraints back on

For what survived: what the kernel's rules leave of it (CLAUDE.md, Goal and design rules;
`.claude/rules/kernel-copy.md`): format path and its decoder cost, work memory per CPU, bounded
decoder, upstream review, zram or seqlz. Price it on the score (`docs/plan.md` §1.1) and say where
on the hull it would land.

### 8. Report

To Martin, in this order: the gap table; what explains the largest gaps; surprises; directions with
their evidence, estimated gain, cost and what decides them next; killed ideas with one line each.
Numbers with what, on which data and hardware. State the log's path.

Designs that were measured go into `docs/explored-designs.md` per CLAUDE.md, in a docs commit on a
branch when Martin asks for it; the log itself stays outside the repository.

## Starting questions

Each is a question about the data or a bound, not a design.

- **Pointers.** What share of a phone page's stored bits are pointer-like 4- and 8-byte values (ART's
  compressed references, native pointers), and what would a model that knows the page's address
  ranges need for them?
- **The per-page ceiling.** How far is seqlz-3 from `xz -9e` per page, and is the rest in the
  literals, the offsets or the parse?
- **Literal structure.** What share of literal bytes is predictable from the byte 4 or 8 before, or
  from the same field of the previous object, and why do the byte-lane tables of September not
  capture it?
- **Decode floor.** For a given parse, which decode instructions per page does any format need
  (copies, one branch per sequence), and how many go to seqlz's format features (bit reading, table
  lookups, coded literals)?
- **Shared state.** Of the 34% that a reference page from the same dump saves (17 Oct), how much is
  in a structure a codec could hold without zram's help?
