#!/bin/bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# check.sh [dir]: docs/format.md against the code, for CI. seqlz_ref.py checks the SHA-256 of every
# table in src/ against the one docs/format.md gives, for 4 KiB and for 16 KiB pages, and then decodes
# pages that src/seqlz.c compressed, with raw and with coded literals: each must give the page back.
# A retrain of the tables without new hashes in docs/format.md fails here, and so does a format change
# that the spec does not describe. The pages are made up, never from a dump: tools/seqlz-worst/pages/,
# and three written here. Needs cc and python3.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"

${CC:-cc} -O2 -DQUETSCHN_PAGE_BITS=12 -I"$repo/src" -o "$out/compress" "$repo/tools/seqlz-viz/compress.c" \
    "$repo/src/seqlz.c" "$repo/src/seqlz_default_tables.c" "$repo/src/seqlz_lit_sets.c"
cp "$repo"/tools/seqlz-worst/pages/*.page "$out/"
python3 - "$out" "$repo/docs/format.md" <<'EOF'
import struct
import sys

out, text = sys.argv[1], open(sys.argv[2], "rb").read()
open(f"{out}/ab.page", "wb").write(b"ab" * 2048)
open(f"{out}/pointers.page", "wb").write(b"".join(struct.pack("<Q", 0x7F1234560010 + 16 * i) for i in range(512)))
open(f"{out}/text.page", "wb").write(text[:4096])
EOF
"$out/compress" "$out"/*.page >/dev/null

# The 16 KiB tables: only their hashes. Loading them checks them, the empty file is no page.
python3 "$here/seqlz_ref.py" --page-bits 14 /dev/null >/dev/null

pages=0
wrong=0
for p in "$out"/*.page; do
    want=$(sha256sum "$p" | cut -d' ' -f1)
    for c in "$p.fast" "$p.lit"; do
        got=$(python3 "$here/seqlz_ref.py" "$c" | cut -d' ' -f2-)
        if [[ "$got" != "valid $want" ]]; then
            echo "$(basename "$c"): $got, the page is $want" >&2
            wrong=$((wrong + 1))
        fi
        pages=$((pages + 1))
    done
done
echo "the tables' hashes match docs/format.md, $wrong of $pages compressed pages decode wrong with seqlz_ref.py"
[[ $wrong -eq 0 ]]
