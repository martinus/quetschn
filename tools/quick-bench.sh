#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# The fast benchmark: zsmalloc cost on the whole corpus without timing, which is exact and takes
# seconds, and latency on a fixed random sample of the corpus, interleaved. Everything is compared with
# the first codec. Run the full benchmark (quetschn-bench-interleaved on the whole corpus) only to
# confirm a result. Nothing else should run on the machine while this one is timing.

set -euo pipefail

usage() {
    cat >&2 <<'EOF2'
usage: tools/quick-bench.sh <build dir> <corpus> <out dir> <codec[:level],...>

The first codec is the baseline. Environment: CPU (default 2) to pin to, SAMPLE (default 20000) pages
for the latency sample, REPETITIONS (default 5) runs per page. The sample is written once, next to the
corpus, as <corpus>-sample<SAMPLE>, and reused.
EOF2
    exit 2
}

[[ $# -eq 4 ]] || usage
build=$1
corpus=$2
out=$3
codecs=$4
cpu=${CPU:-2}
sample_pages=${SAMPLE:-20000}
repetitions=${REPETITIONS:-5}
sample=$corpus-sample$sample_pages

mkdir -p "$out/sizes" "$out/latency"
chmod 700 "$out"
if [[ ! -e $sample.pages ]]; then
    "$build/quetschn-sample-corpus" --corpus "$corpus" --out "$sample" --pages "$sample_pages" >/dev/null
fi

start=$SECONDS
"$build/quetschn-bench-interleaved" --codecs "$codecs" --corpus "$corpus" --no-timing --out "$out/sizes" \
    >"$out/sizes/summary.txt"
"$build/quetschn-bench-interleaved" --codecs "$codecs" --corpus "$sample" --cpu "$cpu" \
    --repetitions "$repetitions" --out "$out/latency" >"$out/latency/summary.txt"

# file name of a codec spec, as quetschn-bench-interleaved writes it: zstd:-1 -> zstd-level-1
file() {
    local name=${1%%:*}
    [[ $1 == *:* ]] && name="$name-level${1#*:}"
    echo "$name"
}

IFS=, read -r -a specs <<<"$codecs"
baseline=$(file "${specs[0]}")
printf '%-16s %8s %9s %22s %22s\n' codec "Σ cost" "raw" "cold p50 / p99 ns" "Δ cold p99 ns [95% CI]"
for spec in "${specs[@]}"; do
    name=$(file "$spec")
    # the block of this codec in a summary: its name, and its level if it has one
    block() {
        awk -v c="${spec%%:*}" -v l="${spec#*:}" -v has_level="$([[ $spec == *:* ]] && echo 1)" '
            $1 == "codec" { cur = $2; sub(",", "", cur); lvl = $4; on = cur == c && (has_level != 1 || lvl == l) && !done }
            on { print } on && $1 == "decompress" && $2 == "cold" { done = 1 }' "$1"
    }
    cost=$(block "$out/sizes/summary.txt" | awk '$1 == "zsmalloc" { print $5; exit }')
    raw=$(block "$out/sizes/summary.txt" | awk '$1 == "stored" { print $3; exit }')
    cold=$(block "$out/latency/summary.txt" | awk '$1 == "decompress" && $2 == "cold" { print $3 " / " $5; exit }')
    delta=""
    if [[ $name != "$baseline" ]]; then
        delta=$("$build/quetschn-compare" --baseline "$out/latency/$baseline.tsv" --candidate "$out/latency/$name.tsv" |
            awk '$1 == "decompress" && $2 == "cold" && $3 == "p99" { print $4 "   " $5 " " $6 }')
    fi
    printf '%-16s %8s %9s %22s %22s\n' "$spec" "$cost" "$raw" "$cold" "$delta"
done
echo "$((SECONDS - start))s, latency on $sample_pages sampled pages, Δ against ${specs[0]}"
