#!/bin/bash
# run.sh <pages> <out dir> <quetschn_<name>.ko> ...: zram's writes and reads in the Mi 9T's own kernel,
# every codec alone in its own zramphone process, in turns, the order rotated per round. With all codecs
# in one process a codec's time depends on the others, by up to 1 us (docs/measuring.md). Writes
# <out dir>/log.txt and prints summary.py's table: the mean over the rounds and the range per codec.
#
#   CODECS   the zram algorithms, default lz4 and seqlz-<name> and seqlz-<name>-lit of each module
#   ROUNDS   default 3
#   CORES    core:kHz, the clock each core is fixed at, default "2:1804800 7:2208000" (A55, A76)
#   BUS=1    the memory's devfreq devices (L3, LLCC, DDR) on performance, at their highest frequency
#   STOP=1   Android stopped while timing (stop/start, adbd keeps running), no UI meanwhile
#   COUNTS=1 kernel instructions, cycles and branch misses per run, with simpleperf
#   COOL     before every run, wait until both clusters are below this temperature, default 45 (C). With
#            BUS=1 a long run heated them to 69 C, and the kernel then held the A76 at 1843 MHz
#   NDK_R21E for zramphone, default ~/opt/android-ndk-r21e, as build.sh
#
# phone.sh does the work on the phone, with these settings in params.sh. Before and after every run a
# STATE line: the cores' clocks, the memory's devfreq frequencies (bus:...), five temperatures.
# Everything is set back at the end, also after an error: the governors as they were, the cores' min and
# max to the hardware's limits. The pages, a copy of a corpus, are deleted from the phone. Needs root on the phone, and lz4 as a module if it is a codec.
set -euo pipefail
[[ $# -ge 3 ]] || { sed -n '2,20p' "$0" >&2; exit 2; }
pages=$1; out=$(realpath -m "$2"); shift 2
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
ndk=${NDK_R21E:-$HOME/opt/android-ndk-r21e}
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang" -O2 -static \
    -o "$out/zramphone" "$here/zramphone.c"
names="" codecs=lz4
for ko in "$@"; do
    n=$(basename "$ko" .ko); n=${n#quetschn_}
    names="$names $n"; codecs="$codecs seqlz-$n seqlz-$n-lit"
done
cat >"$out/params.sh" <<PARAMS
cores="${CORES:-2:1804800 7:2208000}"
codecs="${CODECS:-$codecs}"
mods="${names# }"
rounds=${ROUNDS:-3}
bus=${BUS:-0}
stop=${STOP:-0}
counts=${COUNTS:-0}
cool=${COOL:-45}
PARAMS

D=/data/local/tmp/zram-phone
adb shell "su -c 'rm -rf $D; mkdir -p $D; chmod 777 $D'"
adb push "$@" "$out/zramphone" "$here/phone.sh" "$out/params.sh" $D/ >/dev/null
adb push "$pages" $D/pages >/dev/null
adb shell "su -c 'chmod 755 $D/zramphone; sh $D/phone.sh'" >"$out/log.txt" 2>&1 || true
adb shell "su -c 'rm -rf $D'"
grep -q '^RESTORED' "$out/log.txt" || echo "warning: the phone did not report RESTORED, check it" >&2
python3 "$here/summary.py" "$out/log.txt"
