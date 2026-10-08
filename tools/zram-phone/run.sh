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
#   NDK      for zramphone, default ~/opt/android-ndk-r30
#
# Before and after every run a STATE line: the cores' clocks, the memory's devfreq frequencies, five
# temperatures. Everything is set back at the end, also after an error, and the pages, a copy of a
# corpus, are deleted from the phone. Needs root on the phone, and lz4 as a module if it is a codec.
set -euo pipefail
[[ $# -ge 3 ]] || { sed -n '2,20p' "$0" >&2; exit 2; }
pages=$1; out=$(realpath -m "$2"); shift 2
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
ndk=${NDK:-$HOME/opt/android-ndk-r30}
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang" -O2 -static \
    -o "$out/zramphone" "$here/zramphone.c"
mods=() names=()
for ko in "$@"; do
    mods+=("$ko")
    n=$(basename "$ko" .ko); names+=("${n#quetschn_}")
done
codecs=${CODECS:-lz4$(printf ' seqlz-%s seqlz-%s-lit' $(for n in "${names[@]}"; do echo "$n $n"; done))}
cores=${CORES:-2:1804800 7:2208000}

D=/data/local/tmp/zram-phone
cat >"$out/phone.sh" <<PHONE
# made by tools/zram-phone/run.sh
cd $D || exit 1
cores="$cores"; codecs="$codecs"; mods="${names[*]}"
state() {
	s="STATE \$1"
	# no c or cc here: sh has no local variables, and the loop below uses them
	for sc in \$cores; do s="\$s cpu\${sc%%:*}=\$(cat /sys/devices/system/cpu/cpu\${sc%%:*}/cpufreq/scaling_cur_freq)"; done
	for d in /sys/class/devfreq/soc:qcom,cpu*; do s="\$s \${d##*qcom,}=\$(cat \$d/cur_freq)"; done
	for z in /sys/class/thermal/thermal_zone*; do
		case \$(cat \$z/type) in cpu-0-max-step|cpu-1-max-step|ddr-usr|xo_therm|battery) s="\$s \$(cat \$z/type)=\$(cat \$z/temp)";; esac
	done
	echo "\$s"
}
saved=""
restore() {
	[ "${STOP:-0}" = 1 ] && start
	for x in \$saved; do echo \${x#*=} > \${x%%=*}; done
	[ -n "\${dev:-}" ] && { echo 1 > /sys/block/zram\$dev/reset; echo \$dev > /sys/class/zram-control/hot_remove; }
	for n in \$mods; do rmmod quetschn_\$n 2>/dev/null; done
	rm -f pages zramphone *.ko phone.sh
	state end
	echo "RESTORED"
}
trap restore EXIT
for n in \$mods; do insmod ./quetschn_\$n.ko || exit 1; grep -q "seqlz-\$n-lit" /proc/crypto || exit 1; done
dev=\$(cat /sys/class/zram-control/hot_add)
for cc in \$cores; do
	c=\${cc%%:*}; f=/sys/devices/system/cpu/cpu\$c/cpufreq
	saved="\$f/scaling_governor=\$(cat \$f/scaling_governor) \$f/scaling_max_freq=\$(cat \$f/scaling_max_freq) \$f/scaling_min_freq=\$(cat \$f/scaling_min_freq) \$f/scaling_max_freq=\$(cat \$f/scaling_max_freq) \$saved"
	echo performance > \$f/scaling_governor; echo \${cc#*:} > \$f/scaling_max_freq; echo \${cc#*:} > \$f/scaling_min_freq; echo \${cc#*:} > \$f/scaling_max_freq
done
if [ "${BUS:-0}" = 1 ]; then
	for d in /sys/class/devfreq/soc:qcom,cpu*; do saved="\$d/governor=\$(cat \$d/governor) \$saved"; echo performance > \$d/governor; done
fi
[ "${STOP:-0}" = 1 ] && { stop; sleep 10; }
echo "SETUP rounds ${ROUNDS:-3} bus ${BUS:-0} stop ${STOP:-0} counts ${COUNTS:-0} cool ${COOL:-45} zram\$dev cores \$cores codecs \$codecs"
r=1
while [ \$r -le ${ROUNDS:-3} ]; do
	for cc in \$cores; do
		c=\${cc%%:*}; mask=\$(printf %x \$((1 << c)))
		set -- \$codecs; k=1; while [ \$k -lt \$r ]; do first=\$1; shift; set -- "\$@" \$first; k=\$((k + 1)); done
		for a in "\$@"; do
			echo "RUN round \$r cpu \$c codec \$a"
			w=0
			while [ \$w -lt 120 ]; do
				hot=0
				for z in /sys/class/thermal/thermal_zone*; do
					case \$(cat \$z/type) in cpu-0-max-step|cpu-1-max-step) [ \$(cat \$z/temp) -gt ${COOL:-45}000 ] && hot=1;; esac
				done
				[ \$hot = 0 ] && break
				sleep 5; w=\$((w + 1))
			done
			[ \$w -gt 0 ] && echo "COOLED \$((w * 5)) s"
			state before
			if [ "${COUNTS:-0}" = 1 ]; then
				taskset \$mask simpleperf stat -e instructions:k,cpu-cycles:k,branch-misses:k ./zramphone pages \$dev \$a 2>&1
			else
				taskset \$mask ./zramphone pages \$dev \$a 2>&1
			fi
			state after
		done
	done
	r=\$((r + 1))
done
echo "ALL DONE"
PHONE
adb shell "su -c 'rm -rf $D; mkdir -p $D; chmod 777 $D'"
adb push "${mods[@]}" "$out/zramphone" "$out/phone.sh" $D/ >/dev/null
adb push "$pages" $D/pages >/dev/null
adb shell "su -c 'chmod 755 $D/zramphone; sh $D/phone.sh'" >"$out/log.txt" 2>&1 || true
adb shell "su -c 'rm -rf $D'"
grep -q '^RESTORED' "$out/log.txt" || echo "warning: the phone did not report RESTORED, check it" >&2
python3 "$here/summary.py" "$out/log.txt"
