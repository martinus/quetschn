---
paths:
  - "docs/format.md"
  - "src/seqlz_default_tables*"
  - "src/seqlz_lit_sets*"
  - "src/seqlz_codes.c"
  - "bench/seqlz_counts_*.txt"
  - "bench/seqlz_train*.cpp"
  - "tools/retrain-tables.py"
  - "tools/kernel-port/gen-seqlz-tables.py"
  - "tools/kernel-port/format_rst.py"
  - "tools/seqlz-ref/**"
---

# Format and trained tables

- A format or table change updates `docs/format.md` (rules, SHA-256 table, worked example, Status)
  and both reference decoders, `tools/seqlz-ref/seqlz_ref.py` and `tools/seqlz-ref/seqlz_ref.c`.
  Status = the format as it is now, never a change log (history goes to `docs/explored-designs.md`).
- Reference decoders are written from `docs/format.md` only; never copy from `src/`.
- `tools/seqlz-ref/seqlz_ref.py` parses `docs/format.md` rows ``| `TOK_4k` | `<sha256>` |``: keep
  them exact. Retrained tables need new hashes there (else `check.sh` fails) and new
  `bench/seqlz_counts_*.txt` (`quetschn-seqlz-train --counts`, else test `seqlz_train` fails).
- `.inc` tables: not format-checked, never clang-format or hand-edit. Written by
  `tools/kernel-port/gen-seqlz-tables.py` from `bench/seqlz_counts_*.txt` (command in each file's
  head); `tools/seqlz-ref/check.sh` fails if they differ from its output.
- Retrain: `tools/retrain-tables.py --page-bits 12|14 --train <Release build> --corpus <base>
  --name <name>` (counts, lit lines in committed order, `.inc`, SHA-256 rows of format.md). 4 KiB:
  `~/quetschn-corpus/phone-dump/train-mix`, name `train-mix-1012`, 11 min; 16 KiB:
  `~/quetschn-corpus/emu-a17-16k-a-train`, 20 s. Then by hand: format.md example (`ab` 2048 times
  through `tools/seqlz-compress/compress.c`), its bytes in `test/seqlz_ref_test.cpp`, the book.
- `docs/format.md` is also the kernel's `Documentation/staging/seqlz.rst` via
  `tools/kernel-port/format_rst.py`. HTML comments mark repo-only and kernel-only parts. The converter
  fails on unknown Markdown: block quotes, repo links outside such a mark, `|` or a word ending in `_`
  outside backticks.
- Check: `tools/seqlz-ref/check.sh` (~1.5 s: format.md hashes vs `src/` tables, 22 pages through the
  spec decoder) and `tools/check-links.py`.
