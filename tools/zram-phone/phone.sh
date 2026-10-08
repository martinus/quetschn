# phone.sh: the part of run.sh that runs on the phone, as root, in the directory with the modules,
# zramphone, the pages and params.sh, which run.sh writes: cores, codecs, mods, rounds, bus, stop,
# counts, cool. Sets everything back at the end, also after an error.
cd "$(dirname "$0")" || exit 1
. ./params.sh

# The thermal zones, found once: the two clusters for cool, five more for the STATE lines. Read with
# the shell's read, without a process per file, so that the logging does not move the memory's clocks.
zones=""
for z in /sys/class/thermal/thermal_zone*; do
	read -r t < "$z/type"
	case $t in x86_pkg_temp|cpu-0-max-step|cpu-1-max-step|ddr-usr|xo_therm|battery) zones="$zones $t=$z/temp";; esac
done
state() {
	s="STATE $1"
	for sc in $cores; do read -r v < /sys/devices/system/cpu/cpu${sc%%:*}/cpufreq/scaling_cur_freq; s="$s cpu${sc%%:*}=$v"; done
	for d in /sys/class/devfreq/soc:qcom,cpu*; do read -r v < "$d/cur_freq"; s="$s bus:${d##*qcom,}=$v"; done
	for zt in $zones; do read -r v < "${zt#*=}"; s="$s ${zt%%=*}=$v"; done
	echo "$s"
}
hot() {
	for zt in $zones; do
		case $zt in cpu-0-max-step=*|cpu-1-max-step=*) read -r v < "${zt#*=}"; [ "$v" -gt "${cool}000" ] && return 0;; esac
	done
	return 1
}
# setfreq <cpufreq dir> <min> <max>: the max first, so that the min fits under it, and again at the end,
# in case the old min was above the new max and the first write was refused
setfreq() {
	echo "$3" > "$1/scaling_max_freq" 2>/dev/null; echo "$2" > "$1/scaling_min_freq"; echo "$3" > "$1/scaling_max_freq"
}

saved=""
restore() {
	[ "$stop" = 1 ] && start
	for x in $saved; do
		case $x in
			cpufreq:*) f=${x#cpufreq:}; set -- ${f//,/ }; echo "$2" > "$1/scaling_governor"; setfreq "$1" "$3" "$4";;
			*) echo "${x#*=}" > "${x%%=*}";;
		esac
	done
	[ -n "${dev:-}" ] && { echo 1 > /sys/block/zram$dev/reset; echo "$dev" > /sys/class/zram-control/hot_remove; }
	for n in $mods; do rmmod "quetschn_$n" 2>/dev/null; done
	rm -f pages zramphone ./*.ko phone.sh params.sh
	state end
	echo "RESTORED"
}
trap restore EXIT

for n in $mods; do insmod "./quetschn_$n.ko" || exit 1; grep -q "seqlz-$n-lit" /proc/crypto || exit 1; done
read -r dev < /sys/class/zram-control/hot_add
for cc in $cores; do
	f=/sys/devices/system/cpu/cpu${cc%%:*}/cpufreq
	# back to the hardware's limits at the end, not to what min and max are now: the phone's
	# performance service raises the min for a moment on some events, and a run saved that once
	read -r g < "$f/scaling_governor"; read -r lo < "$f/cpuinfo_min_freq"; read -r hi < "$f/cpuinfo_max_freq"
	saved="cpufreq:$f,$g,$lo,$hi $saved"
	echo performance > "$f/scaling_governor"; setfreq "$f" "${cc#*:}" "${cc#*:}"
done
if [ "$bus" = 1 ]; then
	for d in /sys/class/devfreq/soc:qcom,cpu*; do read -r g < "$d/governor"; saved="$d/governor=$g $saved"; echo performance > "$d/governor"; done
fi
[ "$stop" = 1 ] && { stop; sleep 10; }
perf=""
[ "$counts" = 1 ] && perf="simpleperf stat -e instructions:k,cpu-cycles:k,branch-misses:k"
echo "SETUP rounds $rounds bus $bus stop $stop counts $counts cool $cool zram$dev cores $cores codecs $codecs"
echo "SAVED $saved"
r=1
while [ $r -le "$rounds" ]; do
	for cc in $cores; do
		c=${cc%%:*}; mask=$(printf %x $((1 << c)))
		for a in $codecs; do
			echo "RUN round $r cpu $c codec $a"
			w=0
			while [ $w -lt 120 ] && hot; do sleep 5; w=$((w + 1)); done
			[ $w -gt 0 ] && echo "COOLED $((w * 5)) s"
			state before
			taskset "$mask" $perf ./zramphone pages "$dev" "$a" 2>&1
			state after
		done
	done
	# the first codec goes last, so that each one is in every place once in as many rounds as codecs
	case $codecs in *" "*) codecs="${codecs#* } ${codecs%% *}";; esac
	r=$((r + 1))
done
echo "ALL DONE"
