#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# pc-bench.sh set|restore|show [kHz] [cpu...]: the PC's clock for stable measurements, as
# docs/measuring.md has it. set: boost off, the cpufreq governor performance, the minimum and the maximum
# frequency of each cpu at kHz (default 4500000, 4.5 GHz), cpus default 2, the one tools/zram-vm/run.sh
# and tools/prefetch-vm/run.sh pin their VM to. restore: boost on, the governor powersave, minimum and
# maximum back to the hardware's limits, on every cpu. show: boost, and per cpu the governor and the range.
# set and restore need root: sudo tools/pc-bench.sh set. A reboot also restores. Benchmarks print the
# range and the boost state in their header; a number for a document needs minimum = maximum, boost 0.
set -euo pipefail

sys=/sys/devices/system/cpu
cmd=${1:-show}
shift || true

show() {
    echo "boost: $(cat $sys/cpufreq/boost 2>/dev/null || echo none)"
    for c in "$@"; do
        f=$sys/cpu$c/cpufreq
        echo "cpu$c: $(cat "$f/scaling_governor") $(cat "$f/scaling_min_freq") to $(cat "$f/scaling_max_freq") kHz," \
            "hardware $(cat "$f/cpuinfo_min_freq") to $(cat "$f/cpuinfo_max_freq"), driver $(cat "$f/scaling_driver")"
    done
}

case $cmd in
set)
    khz=${1:-4500000}
    shift || true
    cpus=("${@:-2}")
    [[ $khz =~ ^[0-9]+$ ]] || { echo "kHz, e.g. 4500000: $khz" >&2; exit 2; }
    echo 0 >$sys/cpufreq/boost
    for c in "${cpus[@]}"; do
        f=$sys/cpu$c/cpufreq
        ((khz >= $(cat "$f/cpuinfo_min_freq") && khz <= $(cat "$f/cpuinfo_max_freq"))) ||
            { echo "cpu$c: $khz kHz is outside $(cat "$f/cpuinfo_min_freq") to $(cat "$f/cpuinfo_max_freq")" >&2; exit 2; }
        echo performance >"$f/scaling_governor"
        # down: the minimum first, up: the maximum first, so that the minimum never passes the maximum
        if ((khz < $(cat "$f/scaling_max_freq"))); then
            echo "$khz" >"$f/scaling_min_freq"
            echo "$khz" >"$f/scaling_max_freq"
        else
            echo "$khz" >"$f/scaling_max_freq"
            echo "$khz" >"$f/scaling_min_freq"
        fi
    done
    show "${cpus[@]}"
    ;;
restore)
    echo 1 >$sys/cpufreq/boost
    for f in "$sys"/cpu[0-9]*/cpufreq; do
        echo powersave >"$f/scaling_governor"
        cat "$f/cpuinfo_min_freq" >"$f/scaling_min_freq"
        cat "$f/cpuinfo_max_freq" >"$f/scaling_max_freq"
    done
    show 2
    ;;
show)
    show "${@:-2}"
    ;;
*)
    sed -n '4,11p' "$0" >&2
    exit 2
    ;;
esac
