# seqlz, line by line

A book that explains every line of `src/` to a developer who knows C and C++ but has never written
kernel code or a compressor: the code in pieces, what it does, how at the level of the CPU, and why,
with links to the measurements in [explored-designs.md](../explored-designs.md). CI builds it on every
pull request (job `book`) and publishes it from `main`: <https://martin.ankerl.com/quetschn/>.

```sh
(cd docs/book && npm install)        # KaTeX, for the formulas
python3 docs/book/build.py --check   # writes docs/book/seqlz-line-by-line.html
python3 docs/book/lint.py            # dashes, banned words, measurements without a date or a link
```

- `chapters/*.md`, in the order of `book.json`: Markdown with these additions. `@listing src/<file>
  <from>-<to>` on a line of its own inserts those lines of `src/` as they are in the checkout, with
  their line numbers and a link to them on GitHub. `::: why|kernel|cpu|measured|warn|try <title>`
  up to `:::` is a box. `$...$` outside code is a formula in the text, `$$...$$` on a line of its own
  one by itself; KaTeX renders them at build time, so the page needs no script for them.
  `@anim <name> [wide] <caption>` is an animation, `anim/<name>.js` on the frame `anim/core.js`
  (controls, a sentence per step, both themes); if `anim/<name>.c` exists, it is built with `src/`
  and the JSON it prints is the animation's data, so the numbers are the codec's own;
  `facts=<prefix>,...` after the name adds the facts under those prefixes, the same values the text
  quotes. `shot.py <name> --steps 0,5,last [--narrow] [--dark]` takes screenshots of the built
  animation with headless Chrome and fails if a step scrolls sideways.
  `@stepper decode` and `@stepper match` are the step-throughs of chapters 14 and 9:
  `stepper/instrument.py` puts a hook in front of the statements of a copy of `seqlz_decode()` or
  `seqlz_match_block()`, on the same line, `stepper/trace.c` and `trace_match.c` run the copy on the
  pages, and `stepper.js` and `match.js` play the trace. Both need a C compiler.
- **Values the code computes are computed when the book is built**, never typed: bytes of a
  compressed page, bit strings, codes, counts, sizes, what a program prints. `{{key}}` anywhere in a
  chapter is a value that one of the programs of `facts/` prints (`facts/tables.c`: every code of the
  tables and the sizes; `facts/pages.c`: the example pages of `pages.h`, compressed; one file per
  group of chapters for the rest); `{{key|grp}}` writes `524 912`. A facts file with the line
  `// facts: also 16k` is built for 16 KiB pages too, and its keys come again as `k16.<key>`. A ```` ```c ```` block that is a
  whole program, followed by `@program <name> [-D...] [with=dec_counted]`, is built with `src/` and
  run, and `@output <name> [lines]` shows what it printed. `@assert <expression>` next to a sentence
  that depends on the code without a number in it (`@assert tok.len[0] == 0` for "token 0 has no
  code") fails the build when the sentence stops being true. An unknown key and a failing program stop
  the build with the file and line; the false asserts are all listed at the end, so one change of
  the tables shows every paragraph it touches (swapping the code lengths of two tokens flags 10
  paragraphs in 5 chapters).
- Measured numbers are typed, and stay as they were measured: `lint.py` wants a date or a link in
  their paragraph, or `<!-- undated: why -->` when the numbers are background and not the project's.
- `build.py --check` fails if a non-blank line of `src/` (the generated `.inc` tables aside) is in no
  listing, if an id is on the page twice, or if a link goes to an id that does not exist. `--out <dir>`
  writes the page there instead.
- After a change of `src/`, `remap.py <commit>` moves the listings and the line numbers in the prose
  from `src/` at that commit to `src/` as it is; read what it prints before `--write`.
- `STYLE.md` is the brief the chapters were written to, `REVIEW.md` the one they were checked with.

After a change of the format or the tables, build the book: what the code computes follows by
itself, and a false `@assert` names the paragraph to read again. Prose that explains a value in words
without an assert is the part a change can still make wrong. Needs Python with `markdown` and
`pygments`, Node for `math.mjs`, and a C compiler.
