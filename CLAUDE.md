# quetschn: agent instructions

Read by agents only. Edits stay terse: the rule, plus one short reason where it is not obvious.

Where rules live: this file loads in every session. `.claude/rules/*.md` load when a file matching
their `paths:` is read or edited: `kernel-copy.md` (`src/`, `tools/kernel-port/`),
`format-and-tables.md`, `docs.md`, `book.md` (`src/`, `docs/book/`), `ci.md`. Task skills in
`.claude/skills/`: `issues` ("do the issues"), `research` (bounds, gaps and surprises before
designs), `kernel-patches` (the upstream series).
A new rule goes into the narrowest of these that still loads when it is needed.

Compression codec for Linux zram pages. Codec `seqlz`: one format, three levels of the compressor,
harness names `seqlz-1` (raw literals), `seqlz-2` (one of 8 fixed literal tables, zram's default) and
`seqlz-3` (lazy matcher, a literal table of the page's own); levels by number, so that what a level
does can change. Target: upstream Linux. Project name `quetschn` (binaries, `QUETSCHN_*` options);
codec name stays `seqlz`, never propose a rename.

## Goal and design rules

Goal: near `lz4` speed and `zstd` level 3 (zram's default level for it) ratio. Judge a design by how
much closer it gets, not by whether it arrives.

- Score (`docs/plan.md` §1.1): memory per stored page vs mean time per page written,
  `write + r * read + b * recompression`, r = 0.73 (a phone in daily use, the default; a desktop 0.34).
  Memory = zsmalloc bytes + work memory per CPU (what `kmalloc()`/`vmalloc()` hand out) × CPUs / pages
  in zram, for a named machine (default 8 and 32 CPUs, 64 MiB and 1 GiB). Lower left wins.
  `quetschn-score` computes the hull.
- Kinds of limit (`docs/plan.md` §1, §1.1): hard limit = C6 only, decoder safe and bounded for any
  input. Cost in the score = C5 work memory per CPU (no limit since #203; 16 416 bytes is `lz4`'s, a
  cover-letter number), recompression (`b`). Guard + reported = C2/C3. Reported = C1, C4, global tables.
- C2/C3 (p99 read/write vs `lz4`) are guards, not gates: never drop or keep a design on p99. p99
  clearly worse: find the pages that cause it.
- Recompression: no rule, priced by `b`; measured, not on the hull (explored-designs "Recompression,
  measured, not pursued"). New recompression design: show the hull with `b` stated.
- Codec is pure C. No inline assembly, even for large gains (arm64 asm matcher, -9% cycles on A55,
  rejected, commit `0288b1e`). Empty `__asm__("")` barriers: ask first.
- Format not frozen: format and table changes are allowed (what each one updates:
  `.claude/rules/format-and-tables.md`).
- A new format path or level costs decoder attack surface and upstream review: state its code and
  format cost next to the measured gain; it goes in only with Martin's yes (own literal tables: no on
  9 Oct at 2.5-3.4%, yes on 15 Oct as level 3 at `zstd` 3's size in half its time).

## Layout

```text
src/            codec, freestanding C11, kernel rules: seqlz_compress.c + seqlz_match.h (matcher),
                seqlz_compress_literals.c, seqlz_decompress.c, seqlz_decompress_literals.c,
                seqlz_codes.c (builds the tables; tools/gen-tables/ prints them as C at build time),
                headers seqlz_format.h (format), seqlz_tables.h (tables' layout), seqlz_internal.h
                (what the codec's files share), seqlz.h (userspace API: harness, tests, tools),
                seqlz_default_tables*.{c,inc} + seqlz_lit_sets*.{c,inc} (trained tables = format),
                zram_seqlz.c (zram call shape, harness), seqlz_compat.h (kernel names: u8,
                get_unaligned_le64(), ... outside the kernel)
bench/          C++20 harness, zsmalloc cost model, table training (seqlz_train*.cpp; counts in
                seqlz_counts_*.txt); kernel_codecs/ = zram lz4/lzo/zstd glue built from a kernel tree
test/           doctest, binary quetschn_test; seqlz_endian.c = CI same-bytes check
fuzz/           libFuzzer/AFL++ targets, smoke.sh (CI), afl.sh (long runs); .clusterfuzzlite/ builds them
tools/          see tools/README.md. seqlz-ref/ = reference decoders, kernel-port/ (port.py, KUnit,
                check.sh), zram-vm/ = kernel VM, zram-phone/ = zram in Mi 9T kernel, android-emu/
                (emulator corpus, 4/16 KiB pages), samecode/ (kernel copy's machine code, old vs new)
docs/           plan.md, format.md (spec), seqlz.md (overview), measuring.md (all commands),
                explored-designs.md (every measured design), plots/*.svg; book/ = "seqlz, line by
                line", every line of src/ explained, at the commit in docs/book/book.json
cmake/          kernel_codecs.cmake (kernel flags), check_prefetch.cmake (ctest)
.claude/        rules/ (path-scoped rules), skills/ (issues, research, kernel-patches)
```

## Build, test, format

- Kernel tree: `/home/martinus/gra/linux/finehill`.
- Build in a fresh dir (scratchpad); worktree `build*/` dirs may point at other paths/`_deps`.
- Offline doctest: `-DFETCHCONTENT_SOURCE_DIR_DOCTEST=<some build>/_deps/doctest-src`.
- No `CMAKE_BUILD_TYPE` = Release (`CMakeLists.txt`). Trainer and benchmarks need it: an unoptimized
  trainer took 8x as long. `quetschn-bench-interleaved` (quick-bench, hot loop) only builds with
  `QUETSCHN_KERNEL_TREE`.

```sh
cmake -S . -B $B -G Ninja -DQUETSCHN_WERROR=ON -DQUETSCHN_KERNEL_TREE=/home/martinus/gra/linux/finehill
cmake --build $B && $B/quetschn_test && ctest --test-dir $B      # ctest 2/2 with kernel tree
# 16 KiB pages: -DQUETSCHN_PAGE_BITS=14, no kernel tree (zram glue 4 KiB only)
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.h' '*.c')   # must be 21.1.8 (~/.local/bin)
fuzz/smoke.sh 10 $S/build-fuzz -DFETCHCONTENT_SOURCE_DIR_DOCTEST=...  # clang, ASan+UBSan
fuzz/smoke.sh 10 $S/build-msan -DQUETSCHN_SANITIZE=OFF -DQUETSCHN_MSAN=ON  # MSan, fuzz targets only
tools/gen-tables/gen-tables.sh $S/t.c   # the tables, which every build of src/ outside CMake needs too
gcc -O2 -Isrc -o le test/seqlz_endian.c $S/t.c src/seqlz_{codes,compress,compress_literals,decompress,decompress_literals,default_tables,lit_sets}.c
python3 tools/seqlz-ref/seqlz_ref.py <compressed page>...           # spec decoder, checks table hashes
tools/seqlz-ref/check.sh && tools/check-links.py                    # before pushing docs or tables
K=/home/martinus/gra/linux/finehill   # src/ or tools/kernel-port/ changed:
tools/kernel-port/check.sh $K && tools/kernel-port/check.sh $K ARCH=arm64 LLVM=1 && tools/kernel-port/kunit.sh $K
```

- How much to run: while iterating, the build, `quetschn_test` and `seqlz_endian.c` (its hash stays as on
  main unless the format changes), `port.py` too if `src/` or `tools/kernel-port/` changed. Before a
  push, clang-format, and `check.sh` on x86-64 if the kernel copy changed. CI runs the rest on every
  push (arm64, arm, s390, KUnit, fuzzers): do not repeat it locally. Instruction counts or timing only
  for changes on a hot path.
- CI: `.github/workflows/ci.yml`; `ci-ok` is the only required check. Doctest: read the assertion
  count, not the exit code (zero tests run also exits 0).
- Format: `src/` = `src/.clang-format` (kernel style), rest = root `.clang-format`; the check covers
  every tracked `.c`, also `docs/book/facts/` and `docs/book/anim/`. Python: no formatter in CI; never
  `ruff format` a whole existing file (it rewrites every line), keep its style.
- Tool usage: `usage()` in source or `--help`. Exception: `quetschn-swap-bursts` ignores `--help` and
  starts sampling `/proc/vmstat`; never run it for usage.
- Long parallel jobs (Python `multiprocessing`): start with `setsid nohup ... &`, stop with
  `kill -- -<pgid>`. Killing only the parent leaves its workers running (14 Oct: 30 orphans took half
  the CPUs for 7 h). `pgrep -f`/`pkill -f`/`kill $(pgrep -f X)` also match the own shell's command
  line: write the pattern as `[x]yz`.

## Measuring

Commands: `docs/measuring.md`.

- IMPORTANT: no build, test, fuzz or test program while a benchmark runs, not even a few seconds
  pinned to another core (it happened in two sessions and spoiled the numbers). Check first:
  `pgrep -af '[q]uetschn|[q]emu|[r]un.sh'`.
- Corpus (`corpus/`, `*.pages`) holds keys/passwords: never commit, publish or paste contents; delete
  copies on phones after use.
- Quick bench first: `tools/quick-bench.sh <build> <corpus> <out> lz4,<candidates>` (~90 s, exact
  sizes, latency of 20 000 pages in 5 processes). Full `quetschn-bench-interleaved` or kernel VM only
  to confirm a number for a doc.
- Benchmark header must show min == max frequency and `boost: 0`. Else ask Martin; never change it.
- Compare codecs within one run/boot. Cold-latency difference real only if quick-bench min/max range
  excludes 0. One VM boot can be off by 18%.
- Judges between codecs: kernel VM and the phone's kernel. Userspace = trying out.
- A/B of a branch in the VM: from the branch's worktree, `VARIANTS="main=<worktree of main> same=."
  ALGOS=lz4,seqlz-1,seqlz-2,main-1,main-2,same-1,same-2 BOOTS=3 MODE=swap tools/zram-vm/run.sh`, then
  `tools/zram-vm/variants.py <log>`; `same` = control copy, a difference inside its spread is noise.
- Levels against other codecs in the VM: `ALGOS=lz4,zstd,seqlz-1,seqlz-2,seqlz-3 BOOTS=3 MODE=swap
  tools/zram-vm/run.sh /home/martinus/gra/linux/finehill <base>`, then `quetschn-score <log>` (hull,
  bytes per µs). About 4 min per dump. At most 8 codecs per boot.
- Samples, 20 000 pages each, `~/quetschn-corpus/<name>-sample20000.pages`: `phone-dump/p1` to `p5`
  (p1, p2, p4 are in `train-mix`), `zram0` (desktop, "23rd September"), `zram0-2026-09-24`,
  `zram0-2026-09-28`, `phone-mi9t-2026-10-03`, `phone-mi9t-2026-10-04`. Size counts: all ten, mean
  per row. VM set: p3, p5, `zram0`, `zram0-2026-09-24`.
- Two sizes: summed compressed bytes per page (a page stored raw = 4096) are not zsmalloc memory;
  zsmalloc memory = mean of column `cost` of quick-bench's `<out>/sizes/<codec>.tsv`. Offline layout
  prices (`tools/seqlz-bound/offsets.py`, `space.py`) count token and offset bits only, about 300 B
  per page: 1 B there was about 0.1% of the compressed bytes per page.
- Dev machine (64 GB) never swaps: no live swap sampling there.
- Every measured design: entry in `docs/explored-designs.md` in the same PR as the code (what, numbers,
  code location, kept or not and why), its `Status:` line, its Catalog line; older entries it revisits
  get their `Status:`/Catalog `Now:` updated (format: the file's "Adding or changing an entry"). Before
  proposing a design: read its top (up to `## How the numbers are measured`), then the entries the
  Catalog names. Never read the whole file (~160 000 tokens).

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
- Merge only when Martin says so; "merge when green" = `gh pr checks <n> --watch` in the background,
  then `gh pr merge <n> --merge` once every check of the head passes (`pages` skips on PRs).
- Subagents editing one worktree: one at a time (shared git index). Big doc rewrites: one commit and
  push per chapter, so a stopped run resumes from `git log`.

## Hardware

- Xiaomi Mi 9T (rooted, A76 + A55, kernel 4.14) = test phone.
- Pixel 9a: static binary over `adb` in `/data/local/tmp` only, never root/flash/unlock, clean up
  after; its cycle counter does not run, the harness uses `steady_clock`; `tools/phone-cores.sh` runs
  the codecs per core.
