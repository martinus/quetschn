# seqlz, line by line

A book that explains every line of `src/` to a developer who has never written kernel code or a
compressor: the code in pieces, what each line does, how at the level of C and the CPU, and why,
with links to the measurements in [explored-designs.md](../explored-designs.md).

```sh
(cd docs/book && npm install)        # KaTeX, for the formulas
python3 docs/book/build.py --check   # writes docs/book/seqlz-line-by-line.html
python3 docs/book/lint.py            # dashes and banned words in the prose
```

- `chapters/*.md`, in the order of `book.json`: Markdown with two additions. `@listing src/<file>
  <from>-<to>` on a line of its own inserts those lines of `src/` as they are in the checkout, with
  their line numbers and a link to them on GitHub. `::: why|kernel|cpu|measured|warn|try <title>`
  up to `:::` is a box. `$...$` outside code is a formula in the text, `$$...$$` on a line of its own
  one by itself; KaTeX renders them at build time, so the page needs no script for them.
  `@anim <name> [wide] <caption>` is an animation, `anim/<name>.js` on the frame `anim/core.js`
  (controls, a sentence per step, both themes); if `anim/<name>.c` exists, it is built with `src/`
  and the JSON it prints is the animation's data, so the numbers are the codec's own.
  `@stepper decode` and `@stepper match` are the step-throughs of chapters 14 and 9: `stepper/instrument.py` puts a hook in front of
  the statements of a copy of `seqlz_decode()`, on the same line, `stepper/trace.c` compresses the
  pages with the real compressor and records the copy decoding them, and `stepper/stepper.js` plays
  the trace. It needs a C compiler.
- `build.py --check` fails if a non-blank line of `src/` (the generated `.inc` tables aside) is in no
  listing, and warns about links to ids that do not exist.
- `STYLE.md` is the brief the chapters were written to, `REVIEW.md` the one they were checked with.

The prose quotes line numbers and numbers of the code as it was when it was written; a change of
`src/` keeps the listings right and can make the text around them wrong. Needs Python with
`markdown` and `pygments`, and Node for `math.mjs`.
