# seqlz, bit by bit, and seqlz, compressed

Two web pages that follow real 4 KiB pages one step at a time. `seqlz-bit-by-bit.html` decodes them:
every bit of the compressed page coloured by what it means, the decoder's register and table lookups,
the page filling up, the bits each sequence costs against the same sequence in `lz4`'s format, and
what makes each step fast. `seqlz-compressed.html` goes the other way: the positions the matcher tries,
the hash table filling up, each match grown backwards and forwards, the bits each sequence becomes,
and the literals counted in the 8 tables to see whether coding them pays. They are for learning the
format of [docs/format.md](../../docs/format.md) and how `src/` writes it, by watching. Built copies
are in [docs/](../../docs/seqlz-bit-by-bit.html); a browser opens them as they are.

```sh
tools/seqlz-viz/build.sh            # writes build/seqlz-viz/seqlz-bit-by-bit.html and seqlz-compressed.html
```

Each output is one HTML file with the trace in it, 350 to 450 KB. A link can name the page, the codec
and the step: `#text`, `#heap-fast`, `#relro-s120`.

| file | what it does |
| --- | --- |
| `pages.cpp` | builds a hash map of the words of docs/format.md and dumps a page of its own heap and a page of libstdc++'s relocated data |
| `compress.c` | compresses each page as `seqlz-fast` and `seqlz-fast-lit`, and checks that both decode again |
| `trace.py` | decodes each compressed page with [`seqlz_ref.py`](../seqlz-ref/seqlz_ref.py)'s tables and writes every field with its bit position, and the C decoder's refills and fast path |
| `ctrace.py` | the matcher of `src/page_lz.h` and the encoder of `src/seqlz_compress.c` again, step by step, with every position tried, every hash slot and every field; checks that it writes the bytes `compress.c` wrote |
| `page.html` | the decoder's page, with `/*DATA*/` where the trace goes |
| `compress.html` | the compressor's page, the same |

The pages are real memory of a program run for this, so they contain nothing private, unlike a zram
dump. The heap's addresses differ from run to run, so the sizes differ a little too. `trace.py` and
`ctrace.py` need the Python packages `lz4` and `zstandard`, for the size of the same page in those two.
