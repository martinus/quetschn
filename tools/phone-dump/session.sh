#!/bin/bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# session.sh <name> <minutes> <seed> <urls file> <hog MiB>: one zram dump of a rooted Android phone
# (the Mi 9T) over adb. Reboots the phone, starts hog, runs workload.sh for <minutes>, then writes
# the zram device to $OUT/<name>.raw and the owner of every swap slot, from swapmap, before and after
# the dd, to $OUT/<name>.map1 and .map2. attribute.py turns them into a corpus with processes.
# Needs in /data/local/tmp/qdump: swapmap and hog (NDK clang, -static), start.sh, workload.sh; the documents
# of workload.sh in /sdcard/Download/qdocs.
set -u
name=$1; mins=$2; seed=$3; urls=$4; hog=$5
OUT=${OUT:-$HOME/quetschn-corpus/phone-dump}; mkdir -p "$OUT"
S() { adb shell "su -c '$1'"; }
log() { echo "$(date +%T) $*" | tee -a "$OUT/$name.log"; }
adb push "$urls" /data/local/tmp/urls.txt > /dev/null && adb shell "su -c 'mv /data/local/tmp/urls.txt /data/local/tmp/qdump/'"
adb reboot; sleep 20; adb wait-for-device
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 5; done
sleep 90
log "booted, $(S 'cat /proc/meminfo' | head -1)"
adb shell "su -mm -c 'sh /data/local/tmp/qdump/start.sh $mins $seed $hog'" | tee -a "$OUT/$name.log"
log "workload started: $mins min, seed $seed, $(basename "$urls"), hog $hog MiB"
until S 'test -e /data/local/tmp/qdump/done' 2>/dev/null; do sleep 60; done
adb shell "su -c 'cat /data/local/tmp/qdump/workload.log'" >> "$OUT/$name.log"
S 'pkill -f [w]orkload.sh; pkill -f [c]om.android.commands.monkey; pkill -x hog; sleep 5; cd /data/local/tmp/qdump; ./swapmap > map1; cat /sys/block/zram0/mm_stat'
adb exec-out "su -c 'dd if=/dev/block/zram0 bs=1048576 2>/dev/null'" |
    dd of="$OUT/$name.raw" bs=4096 iflag=fullblock conv=sparse status=none
S 'cd /data/local/tmp/qdump; ./swapmap > map2'
adb exec-out "su -c 'cat /data/local/tmp/qdump/map1'" > "$OUT/$name.map1"
adb exec-out "su -c 'cat /data/local/tmp/qdump/map2'" > "$OUT/$name.map2"
S 'rm -f /data/local/tmp/qdump/map1 /data/local/tmp/qdump/map2'
log "dumped: $(stat -c %s "$OUT/$name.raw") bytes, $(wc -l < "$OUT/$name.map1") / $(wc -l < "$OUT/$name.map2") map lines"
