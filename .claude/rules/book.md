---
paths:
  - "src/**"
  - "docs/book/**"
---

# The book ("seqlz, line by line")

- Before changing or simplifying `src/` code: grep `docs/book/chapters` for the function. The book
  records why code is as it is ("on purpose", "measured"); tell review subagents the same.
- `docs/book/` lists `src/` by line numbers. A `src/` change that moves lines fails CI `book` ("not
  covered") or silently shifts a listing. After the last `src/` edit: `python3 docs/book/remap.py
  <ref>`, `<ref>` = the commit the listings last matched (`origin/main` on a fresh branch, `HEAD` when
  the branch's book was in sync before the edit); it shows moves, `--write` applies them. `--write`
  shifts again on every run: if `src/` changes after it, `git checkout <ref> -- docs/book/chapters`,
  redo hand edits there, remap once more.
- Then reread the prose next to each listing marked CHANGED, then `docs/book/build.py --check`
  (`npm install` in `docs/book` once, or `ln -s` another worktree's `docs/book/node_modules`; git
  ignores it).
- Computed values are `{{key}}` from `docs/book/facts/*.c`, `@program`/`@output` or `@assert`
  (`docs/book/README.md`): format/table changes fail the build where prose needs a look.
- Writing chapters: `docs/book/STYLE.md` (the reader knows C and C++; one number per measurement, the
  mean over the dumps, never per dump). Reviewing chapters: `docs/book/REVIEW.md`. Give subagents the
  matching file.
- New or changed animation: look at it with `docs/book/shot.py <anim> --steps ...`, wide and
  `--narrow`, light and `--dark`.
