#!/bin/bash
# build.sh [dir]: the decoder's and the compressor's visualization, dir/seqlz-bit-by-bit.html and
# dir/seqlz-compressed.html, each one HTML file with everything in it. Three real pages: the heap of
# pages.cpp and libstdc++'s relocated data in it, and 4 KiB of src/seqlz.c's text. Each compressed
# with the tables compiled in, traced by trace.py and ctrace.py, and the traces put into page.html and
# compress.html. Needs a C and a C++ compiler and the Python packages lz4 and zstandard.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-$repo/build/seqlz-viz}
mkdir -p "$out"

${CXX:-c++} -O1 -std=c++20 -o "$out/pages" "$here/pages.cpp"
${CC:-cc} -O2 -DQUETSCHN_PAGE_BITS=12 -I"$repo/src" -o "$out/compress" "$here/compress.c" \
  "$repo/src/seqlz.c" "$repo/src/seqlz_default_tables.c" "$repo/src/seqlz_lit_sets.c"

"$out/pages" "$repo/docs/format.md" "$out"
python3 - "$repo/src/seqlz.c" "$out/text.page" <<'EOF'
import sys
s = open(sys.argv[1], "rb").read()
i = s.index(b"static inline void refill(")
open(sys.argv[2], "wb").write(s[i : i + 4096])
EOF
"$out/compress" "$out/heap.page" "$out/text.page" "$out/relro.page"
python3 "$here/trace.py" "$out"
python3 "$here/ctrace.py" "$out"
# the trace into its page, where the page has /*DATA*/
fill() {
  python3 -c '
import sys
page, data = open(sys.argv[1]).read(), open(sys.argv[2]).read()
assert "/*DATA*/" in page and "</script" not in data
open(sys.argv[3], "w").write(page.replace("/*DATA*/", data))
' "$1" "$2" "$3"
  echo "$3"
}
fill "$here/page.html" "$out/trace.json" "$out/seqlz-bit-by-bit.html"
fill "$here/compress.html" "$out/ctrace.json" "$out/seqlz-compressed.html"
