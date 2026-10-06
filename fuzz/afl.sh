#!/bin/bash
# afl.sh <out dir> [jobs] [cmake args]: builds the fuzz targets with AFL++ and starts them in the
# background, a third of the jobs each on the decoder, the roundtrip and the decoder against the one
# written from FORMAT.md (diff). Per target one main instance, then
# by turns one with ASan and UBSan, which see a read one byte too far, one with CMPLOG for the header's
# magic values, and a plain one. AFL++ from PATH, or from the checkout in $AFL.
#
#   afl-whatsup <out dir>/decode      the state of the instances, also roundtrip and diff
#   pkill -f "afl-fuzz -o <out dir>"  stops them
set -euo pipefail
OUT=$(realpath -m "${1:?usage: afl.sh <out dir> [jobs] [cmake args]}")
JOBS=${2:-9}
CMAKE_ARGS=("${@:3}")
REPO=$(cd "$(dirname "$0")/.." && pwd)
bin() { if [ -n "${AFL:-}" ]; then echo "$AFL/$1"; else command -v "$1"; fi; }

build() { # variant, then the environment afl-clang-lto needs for it
    local b="$OUT/build-$1"
    shift
    env AFL_QUIET=1 "$@" cmake -S "$REPO" -B "$b" -DCMAKE_C_COMPILER="$(bin afl-clang-lto)" \
        -DCMAKE_CXX_COMPILER="$(bin afl-clang-lto++)" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DQUETSCHN_FUZZ=ON "${CMAKE_ARGS[@]}" > /dev/null
    env AFL_QUIET=1 "$@" cmake --build "$b" \
        --target seqlz_decode_fuzz seqlz_roundtrip_fuzz seqlz_diff_fuzz quetschn-fuzz-seeds > /dev/null
}
build plain
build asan AFL_USE_ASAN=1 AFL_USE_UBSAN=1
build cmplog AFL_LLVM_CMPLOG=1
mkdir -p "$OUT"/seeds-{decode,roundtrip}
"$OUT/build-plain/quetschn-fuzz-seeds" "$OUT/seeds-decode" "$OUT/seeds-roundtrip"

# the longest input each target takes, from the page size the build has
page=$((1 << $(sed -n 's/^QUETSCHN_PAGE_BITS:STRING=//p' "$OUT/build-plain/CMakeCache.txt")))
declare -A max_len=([decode]=$((2 * page)) [roundtrip]=$((page + 1)) [diff]=$((2 * page)))
# diff takes compressed pages, as decode does
declare -A seeds=([decode]=decode [roundtrip]=roundtrip [diff]=decode)

export AFL_SKIP_CPUFREQ=1 AFL_NO_UI=1 AFL_TRY_AFFINITY=1
# where core dumps go to a program (systemd-coredump on Fedora), afl-fuzz refuses to start; only root
# can change that, and AFL then may miss a crash that takes the kernel too long to dump
if [[ $(cat /proc/sys/kernel/core_pattern) == \|* ]]; then
    export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1
fi
start() { # target, name, role, extra args
    local t=$1 name=$2 role=$3
    shift 3
    nohup "$(bin afl-fuzz)" -o "$OUT/$t" "$role" "$name" -i "$OUT/seeds-${seeds[$t]}" -t 1000 -G "${max_len[$t]}" "$@" \
        > "$OUT/$t-$name.log" 2>&1 < /dev/null &
}
n=$((JOBS / 3))
for t in decode roundtrip diff; do
    target=seqlz_${t}_fuzz
    start $t main -M -- "$OUT/build-plain/$target"
    for ((k = 1; k < n; k++)); do
        case $((k % 3)) in
        1) start $t asan$k -S -- "$OUT/build-asan/$target" ;;
        2) start $t cmplog$k -S -c "$OUT/build-cmplog/$target" -- "$OUT/build-plain/$target" ;;
        0) start $t plain$k -S -- "$OUT/build-plain/$target" ;;
        esac
    done
done
echo "started $((3 * n)) instances, results in $OUT/decode, $OUT/roundtrip and $OUT/diff"
