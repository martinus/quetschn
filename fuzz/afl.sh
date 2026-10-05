#!/bin/bash
# afl.sh <out dir> [jobs]: builds the fuzz targets with AFL++ and starts them in the background, half
# of the jobs on the decoder, half on the roundtrip. Per target one main instance, then by turns one
# with ASan and UBSan, which see a read one byte too far, one with CMPLOG for the header's magic
# values, and a plain one. AFL is the AFL++ checkout, ~/gra/AFLplusplus/highcoin by default.
#
#   afl-whatsup <out dir>/decode     the state of the instances
#   pkill -f "afl-fuzz -o <out dir>"  stops them
set -euo pipefail
OUT=$(realpath -m "${1:?usage: afl.sh <out dir> [jobs]}")
JOBS=${2:-8}
AFL=${AFL:-$HOME/gra/AFLplusplus/highcoin}
REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC=("$REPO/explore/seqlz.c" "$REPO/explore/seqlz_default_tables.c" "$REPO/explore/seqlz_lit_sets.c")
mkdir -p "$OUT/bin" "$OUT/seeds-decode" "$OUT/seeds-roundtrip"

cc -O2 -I"$REPO/explore" -o "$OUT/bin/make_seeds" "$REPO/fuzz/make_seeds.c" "${SRC[@]}"
"$OUT/bin/make_seeds" "$OUT/seeds-decode" "$OUT/seeds-roundtrip"

for t in decode roundtrip; do
    h="$REPO/fuzz/seqlz_${t}_fuzz.c"
    "$AFL/afl-clang-lto" -O2 -g -I"$REPO/explore" -fsanitize=fuzzer -o "$OUT/bin/$t" "$h" "${SRC[@]}"
    AFL_USE_ASAN=1 AFL_USE_UBSAN=1 "$AFL/afl-clang-lto" -O1 -g -I"$REPO/explore" -fsanitize=fuzzer \
        -o "$OUT/bin/$t-asan" "$h" "${SRC[@]}"
    AFL_LLVM_CMPLOG=1 "$AFL/afl-clang-lto" -O2 -g -I"$REPO/explore" -fsanitize=fuzzer \
        -o "$OUT/bin/$t-cmplog" "$h" "${SRC[@]}"
done

# core_pattern pipes to systemd-coredump on Fedora, which only root can change; AFL then misses
# crashes that the kernel takes too long to dump, ASan aborts are still seen
export AFL_SKIP_CPUFREQ=1 AFL_NO_UI=1 AFL_TRY_AFFINITY=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1
start() { # target, name, role, extra args
    local t=$1 name=$2 role=$3
    shift 3
    nohup "$AFL/afl-fuzz" -o "$OUT/$t" "$role" "$name" -i "$OUT/seeds-$t" -t 1000 "$@" \
        > "$OUT/$t-$name.log" 2>&1 < /dev/null &
}
for t in decode roundtrip; do
    n=$((JOBS / 2))
    start $t main -M -- "$OUT/bin/$t"
    for ((k = 1; k < n; k++)); do
        case $((k % 3)) in
        1) start $t asan$k -S -- "$OUT/bin/$t-asan" ;;
        2) start $t cmplog$k -S -c "$OUT/bin/$t-cmplog" -- "$OUT/bin/$t" ;;
        0) start $t plain$k -S -- "$OUT/bin/$t" ;;
        esac
    done
done
echo "started $JOBS instances, results in $OUT/decode and $OUT/roundtrip"
