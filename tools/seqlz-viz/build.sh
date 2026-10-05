#!/bin/bash
# build.sh [dir]: the decoder visualization, dir/seqlz-bit-by-bit.html, one HTML file with everything
# in it. Three real pages: the heap of pages.cpp and libstdc++'s relocated data in it, and 4 KiB of
# explore/seqlz.c's text. Each compressed with the tables compiled in, traced by trace.py, and the trace
# put into page.html. Needs a C and a C++ compiler and the Python packages lz4 and zstandard.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-$repo/build/seqlz-viz}
mkdir -p "$out"

${CXX:-c++} -O1 -std=c++20 -o "$out/pages" "$here/pages.cpp"
${CC:-cc} -O2 -DQUETSCHN_PAGE_BITS=12 -I"$repo/explore" -o "$out/compress" "$here/compress.c" \
  "$repo/explore/seqlz.c" "$repo/explore/seqlz_default_tables.c" "$repo/explore/seqlz_lit_sets.c"

"$out/pages" "$repo/FORMAT.md" "$out"
python3 - "$repo/explore/seqlz.c" "$out/text.page" <<'EOF'
import sys
s = open(sys.argv[1], "rb").read()
i = s.index(b"static inline void refill(")
open(sys.argv[2], "wb").write(s[i : i + 4096])
EOF
"$out/compress" "$out/heap.page" "$out/text.page" "$out/relro.page"
python3 "$here/trace.py" "$out"
python3 - "$here/page.html" "$out/trace.json" "$out/seqlz-bit-by-bit.html" <<'EOF'
import sys
page, data = open(sys.argv[1]).read(), open(sys.argv[2]).read()
assert "/*DATA*/" in page and "</script" not in data
open(sys.argv[3], "w").write(page.replace("/*DATA*/", data))
EOF
echo "$out/seqlz-bit-by-bit.html"
