#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# run.sh <build dir> <pages file> [out dir]: which branches of seqlz's decoder mispredict on pages it
# sees once. The build dir needs QUETSCHN_KERNEL_TREE: the decoder, seqlz_decompress.c, is compiled with its command from
# compile_commands.json, the kernel's flags, plus -g, which does not change the code. Then perf stat
# per decode in the three modes of decode_once.c, and the mispredicted branches of mode 0 by source
# line from AMD's or Intel's branch records (perf record -j any). x86-64; CPU 2, as the other benchmarks.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build=$(realpath "$1")
pages=$2
out=${3:-$(mktemp -d)}
mkdir -p "$out"

cmd=$(python3 - "$build/compile_commands.json" <<'PY'
import json, sys
for e in json.load(open(sys.argv[1])):
    if "quetschn_kernel_seqlz" in e["command"] and e["file"].endswith("/src/seqlz_decompress.c"):
        print(e["command"])
        break
else:
    sys.exit("no kernel build of src/seqlz_decompress.c in compile_commands.json")
PY
)
cmd=$(sed -E "s# -o [^ ]+# -o $out/seqlz_decompress.o#; s# -c [^ ]+# -c $repo/src/seqlz_decompress.c#" <<<"$cmd")
(cd "$build" && eval "$cmd -g")
cc -O2 -g -I"$repo/src" -o "$out/decode_once" "$here/decode_once.c" "$out/seqlz_decompress.o" \
    "$build/libquetschn_kernel_seqlz.a" "$build/libquetschn_kernel_runtime.a"

for m in 2 0 1; do
    taskset -c 2 perf stat -x, -e branch-misses:u,branches:u,instructions:u,cycles:u \
        "$out/decode_once" "$pages" $m 2>"$out/stat$m.csv" | tee "$out/run$m.txt"
done
taskset -c 2 perf record -q -o "$out/lbr.data" -j any,u -e cycles:u -c 20003 -- "$out/decode_once" "$pages" 0 >/dev/null
perf report -i "$out/lbr.data" -b --stdio --symbols=seqlz_decode,decode_literals --sort mispredict,srcline_from \
    --percent-limit 0 2>/dev/null >"$out/lbr.txt"
# per decode: the counts of mode 0 minus mode 2, and of mode 1 minus mode 0; the mispredicted branches of
# mode 0 by source line, a line's share of them times the misses per decode
python3 - "$out" <<'PY'
import collections, csv, re, sys
out = sys.argv[1]
def stat(m):
    return {r[2]: int(r[0]) for r in csv.reader(open(f"{out}/stat{m}.csv")) if len(r) > 3}
base, once, twice = stat(2), stat(0), stat(1)
n = int(open(f"{out}/run0.txt").read().split()[0])
print(f"{n} pages, per decode:")
for name in base:
    print(f"  {name:16} seen once {(once[name] - base[name]) / n:9.1f}   right after the same page {(twice[name] - once[name]) / n:9.1f}")
lines = collections.Counter()
for l in open(f"{out}/lbr.txt"):
    m = re.match(r"\s*([0-9.]+)%\s+Y\s+(\S+)", l)
    if m:
        lines[m.group(2)] += float(m.group(1))
total = sum(lines.values())
misses = (once["branch-misses:u"] - base["branch-misses:u"]) / n
print("mispredicted branches of a decode seen once, by source line:")
for line, pct in lines.most_common(12):
    print(f"  {line:18} {100 * pct / total:5.1f}%  {misses * pct / total:5.1f} per decode")
PY
