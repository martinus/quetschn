#!/bin/bash
# smoke.sh [seconds] [out dir]: each fuzz target with libFuzzer, ASan and UBSan for the given time, 60 s
# by default, from the seeds of make_seeds.c. For CI; the long runs are afl.sh. Needs clang.
set -euo pipefail
SECONDS_PER_TARGET=${1:-60}
OUT=$(realpath -m "${2:-build-fuzz}")
REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC=("$REPO/explore/seqlz.c" "$REPO/explore/seqlz_default_tables.c" "$REPO/explore/seqlz_lit_sets.c")
CC=${CC:-clang}
mkdir -p "$OUT/seeds-decode" "$OUT/seeds-roundtrip"

"$CC" -O2 -I"$REPO/explore" -o "$OUT/make_seeds" "$REPO/fuzz/make_seeds.c" "${SRC[@]}"
"$OUT/make_seeds" "$OUT/seeds-decode" "$OUT/seeds-roundtrip"
for t in decode roundtrip; do
    "$CC" -O1 -g -I"$REPO/explore" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
        -o "$OUT/$t" "$REPO/fuzz/seqlz_${t}_fuzz.c" "${SRC[@]}"
    mkdir -p "$OUT/corpus-$t"
    # a hang is a failure too: the decoder has to return for any input
    "$OUT/$t" -max_total_time="$SECONDS_PER_TARGET" -timeout=5 -max_len=8200 -print_final_stats=1 \
        -artifact_prefix="$OUT/$t-" "$OUT/corpus-$t" "$OUT/seeds-$t"
done
