#!/bin/bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# check.sh [dir]: docs/format.md against the code, for CI. seqlz_ref.py checks the SHA-256 of every
# table in src/ against the one docs/format.md gives, for 4 KiB and for 16 KiB pages, and then decodes
# pages that src/ compressed, with raw and with coded literals: each must give the page back.
# A retrain of the tables without new hashes in docs/format.md fails here, and so does a format change
# that the spec does not describe. The pages are made up, never from a dump: tools/seqlz-worst/pages/,
# and six written here, three of them with deltas, one for each. First, the tables in src/ must be what tools/kernel-port/gen-seqlz-tables.py
# makes from the counts in bench/, byte for byte. Needs cc and python3.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"

for s in 4k 16k; do
    for t in "--lengths seqlz_default_tables" "--lit-sets seqlz_lit_sets"; do
        set -- $t
        if ! (cd "$repo" && tools/kernel-port/gen-seqlz-tables.py "$1" "bench/seqlz_counts_$s.txt" |
            cmp -s - "src/${2}_$s.inc"); then
            echo "src/${2}_$s.inc is not what tools/kernel-port/gen-seqlz-tables.py $1 makes" >&2
            exit 1
        fi
    done
done
# and it refuses lengths that seqlz_tables_init() refuses, so the kernel's tables built from what it writes
# cannot fail
python3 - "$repo/tools/kernel-port/gen-seqlz-tables.py" <<'EOF'
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("gen", sys.argv[1])
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
gen.check("complete", [1, 2, 3, 3], 3, True)
gen.check("an escaped symbol", [1, 2, 3, 3, 0], 3, False)
for lengths, every in [([1, 2, 2, 2], True), ([1, 2, 3], True), ([1, 2, 4, 4, 4, 4], True), ([1, 2, 3, 3, 0], True)]:
    try:
        gen.check(str(lengths), lengths, 3, every)
    except SystemExit:
        continue
    sys.exit(f"gen-seqlz-tables.py takes the lengths {lengths}, which seqlz_tables_init() refuses")
EOF

${CC:-cc} -O2 -DQUETSCHN_PAGE_BITS=12 -I"$repo/src" -o "$out/compress" "$repo/tools/seqlz-compress/compress.c" \
    "$repo/src/seqlz_codes.c" "$repo/src/seqlz_compress.c" "$repo/src/seqlz_decompress.c" \
    "$repo/src/seqlz_default_tables.c" "$repo/src/seqlz_lit_sets.c"
cp "$repo"/tools/seqlz-worst/pages/*.page "$out/"
python3 - "$out" "$repo/docs/format.md" <<'EOF'
import random
import struct
import sys

out, text = sys.argv[1], open(sys.argv[2], "rb").read()
open(f"{out}/ab.page", "wb").write(b"ab" * 2048)
open(f"{out}/pointers.page", "wb").write(b"".join(struct.pack("<Q", 0x7F1234560010 + 16 * i) for i in range(512)))
open(f"{out}/text.page", "wb").write(text[:4096])
# pages whose literals level 3 codes as deltas, one for each of the three: fields of 4 bytes whose 3 high
# bytes every second field repeats, fields of 8 bytes whose 3 high bytes repeat, and runs of a 3-byte
# pattern, each followed by 8 bytes a little above the byte 6 before them (the last offset 3 counts as 6)
rng = random.Random(229)
p = bytearray()
while len(p) < 4096:
    high = bytes(rng.randrange(256) for _ in range(3))
    p += bytes([rng.randrange(256)]) + high + bytes([rng.randrange(256)]) + high
open(f"{out}/delta4.page", "wb").write(p)
high = bytes(rng.randrange(256) for _ in range(3))
p = b"".join(bytes(rng.randrange(256) for _ in range(5)) + high for _ in range(512))
open(f"{out}/delta8.page", "wb").write(p)
p = bytearray()
while len(p) < 4096:
    p += bytes(rng.randrange(256) for _ in range(3)) * 4
    for _ in range(8):
        p.append((p[-6] + 1 + rng.randrange(3)) & 255)
open(f"{out}/delta-last3.page", "wb").write(p[:4096])
EOF
"$out/compress" "$out"/*.page >/dev/null

# The 16 KiB tables: only their hashes. Loading them checks them, the empty file is no page.
python3 "$here/seqlz_ref.py" --page-bits 14 /dev/null >/dev/null

pages=0
wrong=0
for p in "$out"/*.page; do
    want=$(sha256sum "$p" | cut -d' ' -f1)
    for c in "$p.1" "$p.2" "$p.3"; do
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
