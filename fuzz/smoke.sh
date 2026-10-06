#!/bin/bash
# smoke.sh [seconds] [build dir] [cmake args]: the three fuzz targets at the same time with libFuzzer, ASan and
# UBSan for the given time, 60 s by default, from make_seeds.c's seeds. Inputs that fail end up in
# <build dir>/crashes. For CI; the long runs are afl.sh. Needs clang.
set -euo pipefail
SECONDS_PER_TARGET=${1:-60}
B=$(realpath -m "${2:-build-fuzz}")
REPO=$(cd "$(dirname "$0")/.." && pwd)

cmake -S "$REPO" -B "$B" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF -DQUETSCHN_FUZZ=ON -DQUETSCHN_SANITIZE=ON -DQUETSCHN_WERROR=ON "${@:3}"
cmake --build "$B" --target seqlz_decode_fuzz seqlz_roundtrip_fuzz seqlz_diff_fuzz quetschn-fuzz-seeds
mkdir -p "$B"/seeds-{decode,roundtrip} "$B"/corpus-{decode,roundtrip,diff} "$B/crashes"
"$B/quetschn-fuzz-seeds" "$B/seeds-decode" "$B/seeds-roundtrip"

pids=()
for t in decode roundtrip diff; do
    # diff takes compressed pages, as decode does
    seeds=$([ $t = diff ] && echo decode || echo $t)
    # a hang is a failure too: the decoder has to return for any input
    "$B/seqlz_${t}_fuzz" -max_total_time="$SECONDS_PER_TARGET" -timeout=5 -print_final_stats=1 \
        -artifact_prefix="$B/crashes/$t-" "$B/corpus-$t" "$B/seeds-$seeds" > "$B/$t.log" 2>&1 &
    pids+=($!)
done
status=0
for k in 0 1 2; do
    wait "${pids[$k]}" || status=1
done
tail -n 15 "$B"/decode.log "$B"/roundtrip.log "$B"/diff.log
exit $status
