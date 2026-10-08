#!/bin/bash
# smoke.sh [seconds] [build dir] [cmake args]: the four fuzz targets at the same time with libFuzzer, ASan and
# UBSan for the given time, 60 s by default, from make_seeds.c's seeds. Inputs that fail end up in
# <build dir>/crashes. For CI; the long runs are afl.sh. Needs clang.
set -euo pipefail
SECONDS_PER_TARGET=${1:-60}
B=$(realpath -m "${2:-build-fuzz}")
REPO=$(cd "$(dirname "$0")/.." && pwd)

cmake -S "$REPO" -B "$B" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF -DQUETSCHN_FUZZ=ON -DQUETSCHN_SANITIZE=ON -DQUETSCHN_WERROR=ON "${@:3}"
targets=(decode roundtrip diff encode)
cmake --build "$B" --target $(printf "seqlz_%s_fuzz " "${targets[@]}") quetschn-fuzz-seeds
mkdir -p "$B"/seeds-{decode,roundtrip} "$B/crashes"
"$B/quetschn-fuzz-seeds" "$B/seeds-decode" "$B/seeds-roundtrip"
# diff takes compressed pages, as decode does
ln -sfn seeds-decode "$B/seeds-diff"
# encode starts from nothing: its input is a list of sequences, and runs of zeros are bad ones
mkdir -p "$B/seeds-encode"

pids=()
for t in "${targets[@]}"; do
    mkdir -p "$B/corpus-$t"
    # a hang is a failure too: the decoder has to return for any input
    "$B/seqlz_${t}_fuzz" -max_total_time="$SECONDS_PER_TARGET" -timeout=5 -print_final_stats=1 \
        -artifact_prefix="$B/crashes/$t-" "$B/corpus-$t" "$B/seeds-$t" > "$B/$t.log" 2>&1 &
    pids+=($!)
done
status=0
for pid in "${pids[@]}"; do
    wait "$pid" || status=1
done
for t in "${targets[@]}"; do
    echo "== $t"
    tail -n 15 "$B/$t.log"
done
exit $status
