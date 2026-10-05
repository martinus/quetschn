# seqlz, bit by bit

A web page that decodes real 4 KiB pages one step at a time: every bit of the compressed page coloured
by what it means, the decoder's register and table lookups, the page filling up, the bits each
sequence costs against the same sequence in `lz4`'s format, and what makes each step fast. It is for
learning the format of [FORMAT.md](../../FORMAT.md) by watching it.

```sh
tools/seqlz-viz/build.sh            # writes build/seqlz-viz/seqlz-bit-by-bit.html
```

The output is one HTML file with the trace in it, about 500 KB, that a browser opens as it is. A link
can name the page, the codec and the step: `#text`, `#heap-fast`, `#relro-s120`.

| file | what it does |
| --- | --- |
| `pages.cpp` | builds a hash map of the words of FORMAT.md and dumps a page of its own heap and a page of libstdc++'s relocated data |
| `compress.c` | compresses each page as `seqlz-fast` and `seqlz-fast-lit`, and checks that both decode again |
| `trace.py` | decodes each compressed page with [`seqlz_ref.py`](../seqlz_ref.py)'s tables and writes every field with its bit position, and the C decoder's refills and fast path |
| `page.html` | the page, with `/*DATA*/` where the trace goes |

The pages are real memory of a program run for this, so they contain nothing private, unlike a zram
dump. The heap's addresses differ from run to run, so the sizes differ a little too. `trace.py` needs
the Python packages `lz4` and `zstandard`, for the size of the same page in those two.
