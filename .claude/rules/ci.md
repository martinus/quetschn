---
paths:
  - ".github/**"
  - ".clusterfuzzlite/**"
---

# CI

- `.github/workflows/ci.yml` jobs: format; gcc debug+sanitize; clang release; gcc arm64; gcc 16 KiB
  release+sanitize (no kernel tree); fuzz 60 s/target (4 KiB, 16 KiB, MSan); `same-bytes`
  (`test/seqlz_endian.c` on x86-64, `-m32`, s390x/qemu, outputs diffed); `docs`
  (`tools/seqlz-ref/check.sh`, `tools/check-links.py`); `book` (`docs/book/build.py --check`,
  `lint.py`; job `pages` publishes it from `main` to https://martin.ankerl.com/quetschn/);
  `kernel-port` (`tools/kernel-port/check.sh` x86-64/arm64/arm/s390, `kunit.sh` UML x86-64).
- `ci-ok` is the only required check. Jobs may be renamed; add each new job to `ci-ok`'s `needs` and
  its `test` lines. The test job greps doctest's assertion line: zero tests run = fail.
- `.github/workflows/cflite.yml` (ClusterFuzzLite): daily 1 h each ASan/UBSan/MSan; 10 min ASan on PRs
  touching `src/` or `fuzz/`. Not in `ci-ok`.
