#!/system/bin/sh
# swapbench.sh <out dir> <rounds> [small]: launches the apps round after round, records every launch and the
# swap counters before and after. Run as root with the screen on and no lock screen.
out=$1; rounds=${2:-4}
mkdir -p $out
APPS="com.android.chrome com.google.android.apps.maps com.google.android.youtube com.google.android.apps.photos com.miui.gallery com.google.android.apps.docs com.google.android.videos com.google.android.apps.youtube.music com.facebook.katana com.netflix.mediaclient com.android.settings com.android.thememanager com.mi.globalbrowser com.xiaomi.calendar com.miui.notes com.miui.weather2 com.miui.securitycenter com.miui.calculator com.miui.compass com.miui.videoplayer com.android.contacts com.android.mms com.android.deskclock com.miui.player com.google.android.apps.tachyon"
[ "$3" = small ] && APPS="com.android.chrome com.google.android.apps.maps com.google.android.youtube com.google.android.apps.photos com.facebook.katana com.android.settings com.miui.gallery com.google.android.videos com.google.android.apps.youtube.music com.mi.globalbrowser"
snap() {
  { echo "== $1 $(date +%s)"; grep -E "^(pswpin|pswpout|pgmajfault|pgsteal_kswapd|pgsteal_direct|pgscan_kswapd|pgscan_direct|allocstall|workingset_refault) " /proc/vmstat
    echo "mm_stat $(cat /sys/block/zram0/mm_stat)"; echo "io_stat $(cat /sys/block/zram0/io_stat)"
    for p in $(ps -A -o PID,NAME | awk '$2 ~ /kswapd0/ {print $1}'); do echo "kswapd $(cut -d' ' -f14,15 /proc/$p/stat)"; done
    echo "battery_temp $(dumpsys battery | grep temperature | tr -dc 0-9)"
    # what moves launch times besides the codec: the memory's clocks and the temperatures, as the STATE
    # lines of tools/zram-phone/, read with the shell's read so that this costs no processes
    st="STATE $1"
    for d in /sys/class/devfreq/soc:qcom,cpu*; do read -r v < $d/cur_freq; st="$st bus:${d##*qcom,}=$v"; done
    for zt in $zones; do read -r v < ${zt#*=}; st="$st ${zt%%=*}=$v"; done
    echo "$st"
    grep -E "^(MemFree|MemAvailable|Cached|SwapFree):" /proc/meminfo; } >> $out/stats.txt
}
# the temperatures for snap(), found once
zones=""
for z in /sys/class/thermal/thermal_zone*; do
  read -r t < $z/type
  case $t in cpu-0-max-step|cpu-1-max-step|ddr-usr|xo_therm|battery) zones="$zones $t=$z/temp";; esac
done
logcat -b events -c
snap start
: > $out/launches.txt
r=1
while [ $r -le $rounds ]; do
  for a in $APPS; do
    # Android's cached process limit, which a flag sync after boot sets back to 32
    [ "$(device_config get activity_manager max_cached_processes)" = 96 ] || { device_config put activity_manager max_cached_processes 96; echo "$r $a limit reset" >> $out/limit.txt; }
    comp=$(cmd package resolve-activity --brief $a | tail -1)
    # the pages swapped in and the major faults while the launch runs, columns 5 and 6
    v0=$(awk '$1=="pswpin" || $1=="pgmajfault" {printf "%s ", $2}' /proc/vmstat)
    # am's output to a file and not to a pipe: system_server holds the output open while the launch
    # waits, so a launch stuck on a dialog (a Google sign-in) blocked a pipe for good, timeout or not
    am start -W -n $comp > $out/am.txt 2>&1 &
    k=0; while kill -0 $! 2>/dev/null && [ $k -lt 300 ]; do sleep 0.1; k=$((k + 1)); done
    if kill -0 $! 2>/dev/null; then
      kill -9 $!; echo "$r $a hung" >> $out/limit.txt; input keyevent 4; sleep 1; input keyevent 3; sleep 1
    fi
    res=$(cat $out/am.txt)
    v1=$(awk '$1=="pswpin" || $1=="pgmajfault" {printf "%s ", $2}' /proc/vmstat)
    st=$(echo "$res" | grep LaunchState | cut -d' ' -f2); tt=$(echo "$res" | grep TotalTime | cut -d' ' -f2)
    echo "$r $a ${st:-NONE} ${tt:--1} $(echo $v0 $v1 | awk '{print $3-$1, $4-$2}')" >> $out/launches.txt
    if [ $a = com.android.chrome ] && [ $r = 1 ]; then
      for u in https://en.wikipedia.org/wiki/Linux_kernel https://www.bbc.com/news https://github.com/torvalds/linux https://www.theverge.com; do
        am start -a android.intent.action.VIEW -d $u com.android.chrome > /dev/null 2>&1; sleep 4
      done
    fi
    sleep 3
    input swipe 540 1700 540 600 300; sleep 1; input swipe 540 1700 540 600 300; sleep 1
    input keyevent 3; sleep 1
  done
  snap round$r
  r=$((r + 1))
done
logcat -b events -d | grep -cE "am_kill|am_proc_died" > $out/kills.txt
logcat -b events -d | grep -E "am_kill" > $out/am_kill.txt
logcat -d | grep -iE "lowmemorykiller|lmkd.*kill" | tail -200 > $out/lmkd.txt
snap end
# the same app versions in every run, or the launch times compare different apps; after the run, so
# that 25 dumpsys calls do not come right before the first launch
for a in $APPS; do echo "$a $(dumpsys package $a | grep -m1 versionName | cut -d= -f2)"; done > $out/versions.txt
echo "done"
