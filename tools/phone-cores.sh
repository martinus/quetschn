#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# phone-cores.sh <android build dir> <corpus base> [cpus] [rounds]: the codecs on the cores of a phone
# over adb, in userspace and without root: quetschn-bench-interleaved of an NDK build (docs/measuring.md),
# each codec alone in its own process, pinned to one core, rounds rounds (default 3) with the order rotated,
# for each of cpus (default "7 4 0"). Prints the log that tools/plot-cores.py reads. ANDROID_SERIAL picks
# the phone. Without root the clocks cannot be fixed: the log has each run's clock at its start and end.
# ONCE=1 instead times each page once per pass over all pages, as a swap-in sees it, with the branch
# predictor not trained on the page: --decode-loop 3 (the median of 3 passes) with --compress, --flush
# and --cold, each after 2 s of a busy loop on the core, so that its clock is up when the short passes
# start. That mode does not skip same-filled pages, so give it a corpus without them.
# It sets security.perf_harden to 0 for the cycle counter and back to 1, and deletes the pages and the
# binary from the phone at the end, the pages are private (docs/measuring.md).
set -euo pipefail
[[ $# -ge 2 ]] || { echo "usage: phone-cores.sh <android build dir> <corpus base> [cpus] [rounds]" >&2; exit 2; }
build=$1 corpus=$2 cpus=${3:-7 4 0} rounds=${4:-3}
codecs="lz4 lzo-rle zstd seqlz-1 seqlz-2 seqlz-3"
dir=/data/local/tmp/quetschn-cores
name=$(basename "$corpus")
cleanup() { adb shell "rm -rf $dir; setprop security.perf_harden 1" || true; }
trap cleanup EXIT
adb shell "mkdir -p $dir"
adb push "$build/quetschn-bench-interleaved" "$corpus.pages" "$corpus.tsv" "$dir/" >&2
adb shell "setprop security.perf_harden 0"
for cpu in $cpus; do
    for ((r = 1; r <= rounds; r++)); do
        set -- $codecs
        for ((k = 1; k < r; k++)); do x=$1; shift; set -- "$@" "$x"; done
        for c in "$@"; do
            echo "== cpu$cpu round$r $c temp=$(adb shell dumpsys battery | tr -d '\r' | awk '/temperature/ {print $2}')"
            if [[ ${ONCE:-0} == 1 ]]; then
                mask=$(printf %x $((1 << cpu)))
                for mode in --compress --flush --cold; do
                    adb shell "cd $dir && taskset $mask sh -c 'e=\$((\$(date +%s) + 2)); while [ \$(date +%s) -lt \$e ]; do :; done' &&
                        taskset $mask ./quetschn-bench-interleaved --codecs $c --corpus $name --decode-loop 3 $mode" |
                        tr -d '\r' | grep -E 'per page'
                done
            else
                adb shell "cd $dir && ./quetschn-bench-interleaved --codecs $c --corpus $name --cpu $cpu" |
                    tr -d '\r' | grep -E '^(STATE|timer|zsmalloc cost|compress|decompress)'
            fi
        done
    done
done
