# quetschn: agent instructions

Read by agents only. Keep edits terse: rules and facts, no rationale prose, no human-facing style.

Compression codec for Linux zram pages. Codec `seqlz-fast-lit`: one format, two compressors
(`seqlz-fast` raw literals, `seqlz-fast-lit` coded literals). Target: upstream Linux. Project name
`quetschn` (binaries, `QUETSCHN_*` options); codec name stays `seqlz`, never propose a rename.

## Goal and design rules

Goal: near `lz4` speed and `zstd` level 3 (zram's default level for it) ratio. Judge a design by how
much closer it gets, not by whether it arrives.

- Score (`docs/plan.md` §1.1): memory per stored page vs mean time per page written,
  `write + 0.34 * read + b * recompression`. Memory = zsmalloc bytes + work memory per CPU (what
  `kmalloc()`/`vmalloc()` hand out) × CPUs / pages in zram, for a named machine (default 8 and 32 CPUs,
  64 MiB and 1 GiB). Lower left wins. `quetschn-score` computes the hull.
- Kinds of limit (`docs/plan.md` §1, §1.1): hard limit = C6 only, decoder safe and bounded for any
  input. Cost in the score = C5 work memory per CPU (no limit since #203; 16 416 bytes is `lz4`'s, a
  cover-letter number), recompression (`b`). Guard + reported = C2/C3. Reported = C1, C4, global tables.
- C2/C3 (p99 read/write vs `lz4`) are guards, not gates: never drop or keep a design on p99. p99
  clearly worse: find the pages that cause it.
- Recompression: no rule, priced by `b`; measured, not on the hull (explored-designs "Recompression,
  measured, not pursued"). New recompression design: show the hull with `b` stated.
- Codec is pure C. No inline assembly, even for large gains (arm64 asm matcher, -9% cycles on A55,
  rejected, commit `0288b1e`). Empty `__asm__("")` barriers: ask first.
- Format not frozen: format/table changes allowed. Each one updates `docs/format.md` (rules, SHA-256
  table, worked example, Status) and both reference decoders.

## Layout

```text
src/            codec, freestanding C11, kernel rules: seqlz_compress.c + page_lz.h (matcher),
                seqlz_decompress.c, seqlz_codes.c (table codes), seqlz_internal.h, seqlz.h,
                seqlz_default_tables*.{c,inc} + seqlz_lit_sets*.{c,inc} (trained tables = format),
                zram_seqlz.c (zram call shape, harness), seqlz_compat.h (kernel names: u8,
                get_unaligned_le64(), ... outside the kernel)
bench/          C++20 harness, zsmalloc cost model, table training (seqlz_train*.cpp; counts in
                seqlz_counts_*.txt); kernel_codecs/ = zram lz4/lzo/zstd glue built from a kernel tree
test/           doctest, binary quetschn_test; seqlz_endian.c = CI same-bytes check
fuzz/           libFuzzer/AFL++ targets, smoke.sh (CI), afl.sh (long runs); .clusterfuzzlite/ builds them
tools/          see tools/README.md. seqlz-ref/ = reference decoders from docs/format.md only (never
                copy from src/), kernel-port/, zram-vm/ = kernel VM, zram-phone/ = zram in Mi 9T
                kernel, android-emu/ (emulator corpus, 4/16 KiB pages), samecode/ (kernel copy's
                machine code, old vs new), swap-fault/, phone-apps/, collect/, seqlz-compress/,
                seqlz-worst/, seqlz-bound/, swap-bursts/
docs/           plan.md, format.md (spec), seqlz.md (overview), measuring.md (all commands),
                explored-designs.md (every measured design), plots/*.svg; book/ = "seqlz, line by
                line", every line of src/ explained, built by docs/book/build.py (its README)
cmake/          kernel_codecs.cmake (kernel flags), check_prefetch.cmake (ctest)
.claude/skills/ issues/: "do the issues", order for working the owner's issues to merged PRs
```

Code removed 7 Oct 2026 (`explore/*`, `spike/`): in git before `57fb8fb`
(`git show 57fb8fb^:explore/bytelz.c`). `docs/explored-designs.md` names those paths on purpose.

## The kernel copy

Reviewers read the copy `tools/kernel-port/port.py` writes into a kernel tree, not `src/`. New code
follows these rules; open gaps in existing code: issues #151 to #157 (review of 10 Oct 2026).

- Generated, never edited: fix `src/` or `tools/kernel-port/`. To read it: `git archive` the kernel
  tree's `drivers/block/zram lib/Kconfig lib/Makefile MAINTAINERS
  Documentation/admin-guide/blockdev/zram.rst` into a dir, run `port.py <dir>`.
- Kernel helpers, not own ones: `get_unaligned_le16()`, `sizeof_field()`, `prefetch()`,
  `L1_CACHE_BYTES`, `BIT()`. No inline assembly, not even per architecture: fix a kernel gap with its
  own kernel patch, carried in `tools/kernel-port/` and applied by `port.py` (`x86-prefetcht0.patch`).
  `src/seqlz_compat.h` = kernel headers outside the kernel; may do what such a patch does.
- Nothing in `lib/` depends on the CPU model; `port.py` fails on `read_cpuid` or `MIDR_` there (#152:
  one decode loop for all cores).
- `static` unless zram calls it. Grep-able names (not `count`, `refill`). Every number named or
  commented with its origin.
- Comments: what the code does, plus one line of why. Measurement numbers (phone, compiler, dumps) go
  to docs/explored-designs.md and the commit message, not `src/` (#156). When changing code, check
  the comments and kernel-doc above it (the 10 Oct review found 5 wrong comments, 6 dead file names).
- Every kernel path runs in KUnit; every decoder path is reached by a fuzz target.
- Memory = what `kmalloc()`/`vmalloc()` hands out, not what is asked: 8192 + 4112 bytes in two
  allocations = 16 KiB, as one union = 8 KiB (#154).
- `tools/kernel-port/check.sh`: W=1 warning-free on x86-64, arm64, arm, s390; no new checkpatch
  `--strict` finding; stack frames < 1024 bytes (older kernels' 32-bit `FRAME_WARN`; now 1280, 2048 on
  64-bit). It prints the three deepest and fails at 1024. Deepest: `code_literals()` and
  `decode_literals()`, 640 (gcc 16.2.1, x86-64 defconfig, `W=1`), 1000 and 952 with KASAN+UBSan as
  `stress.sh` sets them.
- The copy never names this repo, our machines, phones or dumps. `port.py` fails on names
  (`FORBIDDEN`), not on wording like "on pages of a phone"; also on a `REWRITES` entry that matches no
  comment any more (change or remove it with its comment).

## Kernel patches (Phase 7)

Source: kernel Maintainers Summit, Oct 2026. Never quote, cite or link its report.

- Changelogs and cover letter: Martin writes them. You supply code, tests, numbers, bullet points.
  Changelog = problem, change, measured result; short (padded text reads as tool output).
- Tool use: state it in the cover letter only (tools, which step, Martin reviewed and tested all).
  Before sending, check `Documentation/process/coding-assistants.rst` in the base tree for
  `Assisted-by:` (summit agreed to drop the requirement; the file decides). Martin picks the wording.
  The "no Claude attribution" rule covers this repo and GitHub, not the cover letter.
- Before each version: "The kernel copy" rules hold; `tools/kernel-port/kunit.sh` passes on the exact
  base; build with newest gcc and clang. Humans review why/where/taste, so the cover letter answers
  "why a new codec" and "why this place in the tree" up front.
- Before each version: cleanup pass over the kernel files. Comments match code; no dead code, unused
  option or unneeded function; less code wins. Backend API follows the other zram backends.
  Martin must be able to explain every line.
- Error paths: when code changes, update KUnit error-path tests too (`seqlz_test_damaged`).
- Resend policy: never one version per bot (Sashiko) finding or per mail. Wait for human design
  comments, answer all in one version. Wrong bot finding: Martin explains why. Bot finding in code the
  series does not touch: separate fix if real, else ignore.
- Replies to reviewers: Martin, in person, soon, plain words; never pasted tool output. Fix small
  nits (include order, names) without discussion; maintainers use them to test for a human.
- Format: `git format-patch --base=<commit>` on the tree `MAINTAINERS` names for zram at send time;
  name that tree in the cover letter. After "applied", check linux-next, do not ask.
- After merge, decoder bug = security bug: kernel security process, no disclosure to anyone (employer
  included) before the fix is public, fix upstream fast, `Fixes:` + `Cc: stable@vger.kernel.org`.

## Build, test, format

- Kernel tree: `/home/martinus/gra/linux/finehill`.
- Build in a fresh dir (scratchpad); worktree `build*/` dirs may point at other paths/`_deps`.
- Offline doctest: `-DFETCHCONTENT_SOURCE_DIR_DOCTEST=<some build>/_deps/doctest-src`.

```sh
cmake -S . -B $B -G Ninja -DQUETSCHN_WERROR=ON -DQUETSCHN_KERNEL_TREE=/home/martinus/gra/linux/finehill
cmake --build $B && $B/quetschn_test && ctest --test-dir $B      # 133 cases, ctest 2/2 with kernel tree
# 16 KiB pages: -DQUETSCHN_PAGE_BITS=14, no kernel tree (zram glue 4 KiB only), 112 cases
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.h' '*.c')   # must be 21.1.8 (~/.local/bin)
fuzz/smoke.sh 10 $S/build-fuzz -DFETCHCONTENT_SOURCE_DIR_DOCTEST=...  # clang, ASan+UBSan
fuzz/smoke.sh 10 $S/build-msan -DQUETSCHN_SANITIZE=OFF -DQUETSCHN_MSAN=ON  # MSan, fuzz targets only
gcc -O2 -Isrc -o le test/seqlz_endian.c src/seqlz_{codes,compress,decompress,default_tables,lit_sets}.c
python3 tools/seqlz-ref/seqlz_ref.py <compressed page>...           # spec decoder, checks table hashes
```

- CI `.github/workflows/ci.yml` jobs: format; gcc debug+sanitize; clang release; gcc arm64; gcc 16 KiB
  release+sanitize (no kernel tree); fuzz 60 s/target (4 KiB, 16 KiB, MSan); `same-bytes`
  (seqlz_endian.c on x86-64, `-m32`, s390x/qemu, outputs diffed); `docs` (`tools/seqlz-ref/check.sh`,
  `tools/check-links.py`); `book` (`docs/book/build.py --check`, `lint.py`; job `pages` publishes it
  from `main` to https://martin.ankerl.com/quetschn/); `kernel-port` (`tools/kernel-port/check.sh`
  x86-64/arm64/arm/s390, `kunit.sh` UML x86-64).
- `ci-ok` = only required check. Jobs may be renamed; add each new job to `ci-ok`'s `needs` and its
  `test` lines. Test job greps doctest's assertion line: zero tests run = fail.
- `.github/workflows/cflite.yml` (ClusterFuzzLite): daily 1 h each ASan/UBSan/MSan; 10 min ASan on PRs
  touching `src/` or `fuzz/`. Not in `ci-ok`.
- `src/` style = `src/.clang-format` (kernel: tabs, 80 cols, `u8 *p`). Comments not reflowed: wrap by
  hand to 80 cols, kernel block form. Rest of repo: root `.clang-format`.
- `.inc` tables: not format-checked, never clang-format or hand-edit. Written by
  `tools/kernel-port/gen-seqlz-tables.py` from `bench/seqlz_counts_*.txt` (command in each file's
  head); `tools/seqlz-ref/check.sh` fails if they differ from its output.
- Before pushing a docs or table change: `tools/seqlz-ref/check.sh` (~1.5 s: format.md hashes vs
  `src/` tables, 22 pages through spec decoder) and `tools/check-links.py`.
- Tool usage: `usage()` in source or `--help`. Exception: `quetschn-swap-bursts` ignores `--help` and
  starts sampling `/proc/vmstat`; never run it for usage.
- No build/test/fuzz while a benchmark runs: `pgrep -af 'quetschn|qemu|run.sh'` first.

## Measuring

Commands: `docs/measuring.md`.

- Quick bench first: `tools/quick-bench.sh <build> <corpus> <out> lz4,<candidates>` (~90 s, exact
  sizes, latency of 20 000 pages in 5 processes). Full `quetschn-bench-interleaved` or kernel VM only
  to confirm a number for a doc.
- Benchmark header must show min == max frequency and `boost: 0`. Else ask Martin; never change it.
- Compare codecs within one run/boot. Cold-latency difference real only if quick-bench min/max range
  excludes 0. One VM boot can be off by 18%.
- Judges between codecs: kernel VM and the phone's kernel. Userspace = trying out.
- Dev machine (64 GB) never swaps: no live swap sampling there.
- Corpus (`corpus/`, `*.pages`) holds keys/passwords: never commit, publish or paste contents; delete
  copies on phones after use.
- Every measured design: entry in `docs/explored-designs.md` in the same PR as the code (what, numbers,
  code location, kept or not and why) + Index at top. Read the file before proposing a design.

## Editing docs: invariants that break silently

- Code comments, scripts, SVG comments cite `docs/plan.md` sections (§1.1, §3.1, §3.3, §3.5, §4, §5.1,
  §5.2, §5.3, Phase 1, Phase 4). Never renumber.
- Anchors used across files (`plan.md#11-the-score-memory-against-time-not-bars`, `format.md#status`,
  `format.md#example`, many `explored-designs.md#...`). Never rename headings without fixing links.
  After Markdown edits: `tools/check-links.py`.
- `tools/seqlz-ref/seqlz_ref.py` parses `docs/format.md` rows ``| `TOK_4k` | `<sha256>` |``:
  keep exact. Retrained tables need new hashes there (else `check.sh` fails) and new
  `bench/seqlz_counts_*.txt` (`quetschn-seqlz-train --counts`, else test `seqlz_train` fails).
- `docs/format.md` = also the kernel's `Documentation/staging/seqlz.rst` via
  `tools/kernel-port/format_rst.py`. HTML comments mark repo-only and kernel-only parts. Converter
  fails on unknown Markdown: block quotes, repo links outside such a mark, `|` or a word ending in `_`
  outside backticks.
- Doc numbers go stale when tables/matcher change. Verify before quoting: `seqlz_ref.load_tables(12)`
  (from `tools/seqlz-ref/`) gives codes; `tools/seqlz-compress/compress.c` (`-Isrc`) compresses a page
  file. Name the tables used ("tables of 7 October").
- Codec charts: x = time per page, y = memory as % of page, both from 0.
- `docs/book/` lists `src/` by line numbers. A `src/` change that moves lines fails CI `book` ("not
  covered") or silently shifts a listing. Same PR: `python3 docs/book/remap.py origin/main` (shows
  moves; `--write` applies), reread the prose next to each listing marked CHANGED, then
  `docs/book/build.py --check` (`npm install` in `docs/book` once). Computed values are `{{key}}` from
  `docs/book/facts/*.c`, `@program`/`@output` or `@assert` (docs/book/README.md): format/table
  changes fail the build where prose needs a look.

## Writing and git

- Public text (commits, PR title/body, GitHub comments, code comments, docs): Martin's voice, read
  `~/gra/martin-voice.md` fresh each time. No em dashes, `e.g.`, numbers with axis and hardware, state
  the loss, caveat last.
- Never add Claude attribution, footer, `Co-Authored-By` or session link in the repo or on GitHub.
- Commits: `area: what changed` (`seqlz:`, `docs:`, `tools:`, `test:`); body why/how/measured;
  always `git commit -s`.
- `gh` works on this machine. Martin merges PRs while you work: `gh pr view <n> --json state` right
  before pushing to or editing a PR; if merged, new branch from `origin/main`.
- Repo and worktrees via `gra` (global instructions). `gra -y done <name>` allowed for finished,
  merged, clean worktrees, never the session's own.
- Hardware: Xiaomi Mi 9T (rooted, A76 + A55, kernel 4.14) = test phone. Pixel 9a: static binary over
  `adb` in `/data/local/tmp` only, never root/flash/unlock, clean up after.
