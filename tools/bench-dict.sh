#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# Trains a dictionary on one corpus and measures all zram codecs on another, with and without it.
# Meant for two zram dumps taken days apart, see README.md. PLAN.md §5.3 for why the dictionary must
# not see the pages it is measured on.

set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage: tools/bench-dict.sh <build dir> <train corpus> <test corpus> <out dir>

Corpora are <base> paths, as for quetschn-bench-* --corpus. Pages of the train corpus that also are
in the test corpus are not used for training. Environment: CPU (default 2) is the CPU to pin to,
REPETITIONS (default 5) the runs per page, MAXDICT (default 64KB) the dictionary size.
EOF
    exit 2
}

[[ $# -eq 4 ]] || usage
build=$1
train=$2
test=$3
out=$4
cpu=${CPU:-2}
repetitions=${REPETITIONS:-5}
maxdict=${MAXDICT:-64KB}

mkdir -p "$out"
chmod 700 "$out"
"$build/quetschn-split-corpus" --corpus "$train" --train "$out/train" --exclude "$test"
zstd -q -f --train "$out/train.pages" -B4096 --maxdict="$maxdict" -o "$out/dict"

# name, binary, extra arguments
runs=(
    "lz4 lz4"
    "lz4-dict lz4 --dict $out/dict"
    "lzo-rle lzo-rle"
    "lzo lzo"
    "zstd-1 zstd --level -1"
    "zstd-1-dict zstd --level -1 --dict $out/dict"
    "zstd3 zstd --level 3"
    "zstd3-dict zstd --level 3 --dict $out/dict"
)
for run in "${runs[@]}"; do
    read -r name codec args <<<"$run"
    echo "== $name"
    # shellcheck disable=SC2086 # args is split on purpose
    "$build/quetschn-bench-$codec" --corpus "$test" --cpu "$cpu" --repetitions "$repetitions" $args \
        --out "$out/$name.tsv" >"$out/$name.txt"
    grep -E '^zsmalloc cost' "$out/$name.txt"
done

# baseline, candidate
pairs=(
    "lzo-rle lz4"
    "lzo-rle lz4-dict"
    "lz4 lz4-dict"
    "lzo-rle zstd-1"
    "lz4-dict zstd-1"
    "zstd-1 zstd-1-dict"
    "zstd3 zstd3-dict"
)
for pair in "${pairs[@]}"; do
    read -r baseline candidate <<<"$pair"
    "$build/quetschn-compare" --baseline "$out/$baseline.tsv" --candidate "$out/$candidate.tsv" \
        >"$out/compare-$baseline-$candidate.txt"
    printf '%-10s -> %-12s %s\n' "$baseline" "$candidate" \
        "$(grep -E '^candidate saves' "$out/compare-$baseline-$candidate.txt" | sed 's/^candidate saves *//')"
done
