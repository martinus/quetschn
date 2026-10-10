# seqlz, line by line

A book that explains every line of `src/` to a developer who has never written kernel code or a
compressor: the code in pieces, what each line does, how at the level of C and the CPU, and why,
with links to the measurements in [explored-designs.md](../explored-designs.md).

```sh
python3 docs/book/build.py --check   # writes docs/book/seqlz-line-by-line.html
python3 docs/book/lint.py            # dashes and banned words in the prose
```

- `chapters/*.md`, in the order of `book.json`: Markdown with two additions. `@listing src/<file>
  <from>-<to>` on a line of its own inserts those lines of `src/` as they are in the checkout, with
  their line numbers and a link to them on GitHub. `::: why|kernel|cpu|measured|warn|try <title>`
  up to `:::` is a box.
- `build.py --check` fails if a non-blank line of `src/` (the generated `.inc` tables aside) is in no
  listing, and warns about links to ids that do not exist.
- `STYLE.md` is the brief the chapters were written to, `REVIEW.md` the one they were checked with.

The prose quotes line numbers and numbers of the code as it was when it was written; a change of
`src/` keeps the listings right and can make the text around them wrong. Needs Python with
`markdown` and `pygments`.
