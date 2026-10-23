---
paths:
  - "src/**"
  - "docs/book/**"
---

# The book ("seqlz, line by line")

- Before changing or simplifying `src/` code: grep `docs/book/chapters` for the function. The book
  records why code is as it is ("on purpose", "measured"); tell review subagents the same.
- The book describes the code of one commit, `"src"` in `docs/book/book.json`, and builds from it, not
  from the checkout. A `src/` change does not touch the book: no remap, no prose, CI `book` stays
  green. Update the book only when Martin asks for it, in a PR of its own (one pass for many changes
  is cheaper than one per change).
- Book update: on a branch whose code is committed, `python3 docs/book/remap.py [<commit>]` (default
  `HEAD`) shows how listings move from the book's commit; `--write` applies them and moves `"src"`.
  Then reread the prose next to each listing marked CHANGED, grep the chapters for every name the
  changes removed or renamed, then `docs/book/build.py --check` (`npm install` in `docs/book` once, or
  `ln -s` another worktree's `docs/book/node_modules`; git ignores it).
- Computed values are `{{key}}` from `docs/book/facts/*.c`, `@program`/`@output` or `@assert`
  (`docs/book/README.md`): format/table changes fail the build where prose needs a look.
- Writing chapters: `docs/book/STYLE.md` (the reader knows C and C++; one number per measurement, the
  mean over the dumps, never per dump). Reviewing chapters: `docs/book/REVIEW.md`. Give subagents the
  matching file.
- New or changed animation: look at it with `docs/book/shot.py <anim> --steps ...`, wide and
  `--narrow`, light and `--dark`.
