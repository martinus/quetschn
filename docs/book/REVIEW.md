# Review brief for "seqlz, line by line"

You are a technical reviewer of chapters written by other agents. Book directory: this file's
directory (BOOK), `docs/book/` of this repository; do not edit anything outside it.
Read BOOK/STYLE.md first: it is the brief the writers had. The reader is a developer who never did
kernel or compression work and wants to understand every line of src/, at depth.

For each chapter assigned to you, in this order of importance:

1. **Correctness.** Check every technical claim against the code, the docs (docs/format.md,
   docs/seqlz.md, docs/plan.md, docs/explored-designs.md) and `git log`. Wrong semantics of a C line,
   a wrong bit position, a wrong number, a measurement attributed to the wrong hardware or date, a
   reason the repository does not give presented as fact. Re-run the worked examples that matter
   (compile small programs against src/ with `gcc -O2 -Isrc src/seqlz_codes.c src/seqlz_compress.c
   src/seqlz_decompress.c src/seqlz_default_tables.c src/seqlz_lit_sets.c` in a scratch directory outside the repository, or python3; tools/seqlz-ref/seqlz_ref.py has load_tables(12)). Check that
   numbers inside SVG diagrams match the text. Links to GitHub docs: the anchor must be the GitHub
   slug of an existing heading (lowercase, drop characters that are not letters, digits, spaces,
   hyphens or underscores, spaces to hyphens).
2. **Every line explained.** After each `@listing`, every line of it is explained or obviously
   covered by the prose. A listing whose lines are only paraphrased, or skipped, gets the missing
   explanation added (what, how in C/CPU terms, why).
3. **Clarity for the newcomer.** A term used before it is explained (in this chapter or an earlier
   one, by link) gets a short explanation or a link. A paragraph that is confusing gets rewritten.
   Contradictions with other chapters (e.g. a number or a reason given differently elsewhere): grep
   the other chapters, fix the one that is wrong, or report it if it is outside your chapters.
4. **Voice.** STYLE.md's rules: no em/en dashes in prose, `e.g.`, no banned words, no hype, caveat
   last, plain words.

Fix what you find directly in your assigned chapter files, with minimal, targeted edits that keep the
writer's structure and voice. Do not rewrite chapters wholesale and do not shorten them for its own
sake. Keep every `@listing` line (coverage must stay complete). After editing, run
`python3 build.py` in BOOK: it must print no `not covered:` and no `warning:` lines, and
`python3 lint.py` should print at most the "magic bytes" line. Only edit your own chapters; other
reviewers work on the others at the same time.

Final answer: a list of the changes you made (chapter, what was wrong, what it says now), and a
separate list of doubts you could not resolve or findings about the repository itself (e.g. a code
comment that says something the code does not do), with evidence.
