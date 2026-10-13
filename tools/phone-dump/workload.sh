#!/system/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# workload.sh <minutes> <seed> <urls file>: uses the apps of a rooted phone for <minutes>, so that zram
# fills with the pages of apps as a person's use would fill it: web pages in two browsers, videos,
# maps of many places, photos and videos taken with the camera, documents, and random taps (monkey)
# in the apps with content. Every app round goes through the apps in another order, from <seed>.
# Run as root from /data/local/tmp/qdump, with hog holding RAM. Logs to stdout.
mins=$1; seed=$2; urls=$3
end=$(( $(date +%s) + mins * 60 ))
RANDOM=$seed
WORDS="kernel bread garden train piano river castle coffee football election recipe guitar weather
mountain bicycle hospital museum camera island festival chess painting soup volcano satellite library
tomato jazz marathon dinosaur bridge forest robot pizza ocean violin tennis desert planet theatre"
YT="dQw4w9WgXcQ jNQXAC9IVRw 9bZkp7q19f0 kJQP7kiw5Fk OPf0YbXqDm0 aqz-KE-bpKQ YE7VzlLtp-4"
CONTENT="com.google.android.youtube com.google.android.apps.maps com.android.chrome com.mi.globalbrowser
com.google.android.apps.magazines com.google.android.googlequicksearchbox com.xiaomi.mipicks
com.android.thememanager com.miui.weather2 com.miui.gallery com.miui.notes cn.wps.xiaomi.abroad.lite
com.google.android.apps.youtube.music com.miui.videoplayer com.miui.player com.google.android.videos
com.miui.android.fashiongallery com.google.android.apps.photos com.miui.calculator com.xiaomi.calendar"
OTHER="com.android.settings com.android.contacts com.android.mms com.miui.securitycenter com.miui.cleanmaster
com.android.camera com.google.android.gm com.netflix.mediaclient com.facebook.katana com.google.android.apps.docs
com.android.vending com.mi.android.globalFileexplorer com.miui.compass"
pick() { set -- $1; shift $(( RANDOM % $# )); echo $1; }
nurls=$(wc -l < $urls)
url() { sed -n "$(( RANDOM % nurls + 1 ))p" $urls | sed "s/WORD/$(pick "$WORDS")+$(pick "$WORDS")/"; }
guard() {
  svc wifi enable; svc power stayon usb
  [ "$(settings get global airplane_mode_on)" = 0 ] || { settings put global airplane_mode_on 0; am broadcast -a android.intent.action.AIRPLANE_MODE > /dev/null 2>&1; }
  dumpsys power 2>/dev/null | grep -c 'mWakefulness=Awake' > /dev/null || input keyevent 224
  [ "$(device_config get activity_manager max_cached_processes)" = 96 ] || device_config put activity_manager max_cached_processes 96
}
scroll() { for i in $(seq $1); do input swipe 540 1800 540 700 250; sleep 2; done; }
launch() { c=$(cmd package resolve-activity --brief $1 | tail -1); am start -n $c > /dev/null 2>&1; }
monkey_in() { timeout -s KILL 120 monkey -p $1 -s $RANDOM --throttle 250 --pct-syskeys 0 --pct-appswitch 0 --pct-anyevent 0 --pct-nav 0 --pct-majornav 5 --pct-touch 55 --pct-motion 30 $2 > /dev/null 2>&1; }
web() { for i in $(seq $2); do u=$(url); am start -a android.intent.action.VIEW -d "$u" $1 > /dev/null 2>&1; sleep 10; scroll 3; done; }
use() {
  case $1 in
  com.android.chrome|com.mi.globalbrowser) web $1 4; monkey_in $1 150 ;;
  com.google.android.youtube)
    am start -a android.intent.action.VIEW -d "https://www.youtube.com/watch?v=$(pick "$YT")" $1 > /dev/null 2>&1; sleep 25
    am start -a android.intent.action.SEARCH -e query "$(pick "$WORDS") $(pick "$WORDS")" $1 > /dev/null 2>&1; sleep 8; scroll 3; monkey_in $1 100 ;;
  com.google.android.apps.maps)
    for i in 1 2 3; do
      lat=$(( RANDOM % 120 - 60 )).$(( RANDOM % 1000 )); lon=$(( RANDOM % 340 - 170 )).$(( RANDOM % 1000 ))
      am start -a android.intent.action.VIEW -d "geo:0,0?q=$(pick "$WORDS")" $1 > /dev/null 2>&1; sleep 8
      am start -a android.intent.action.VIEW -d "geo:$lat,$lon?z=$(( RANDOM % 8 + 10 ))" $1 > /dev/null 2>&1; sleep 8; scroll 2
    done ;;
  com.android.camera)
    am start -a android.media.action.STILL_IMAGE_CAMERA > /dev/null 2>&1; sleep 4; input keyevent 27; sleep 3; input keyevent 27; sleep 3
    am start -a android.media.action.VIDEO_CAMERA > /dev/null 2>&1; sleep 4; input keyevent 27; sleep 10; input keyevent 27; sleep 3 ;;
  cn.wps.xiaomi.abroad.lite)
    f=$(ls /sdcard/Download/qdocs/* | sed -n "$(( RANDOM % $(ls /sdcard/Download/qdocs | wc -l) + 1 ))p")
    am start -a android.intent.action.VIEW -d "file://$f" -t "$(case $f in *.pdf) echo application/pdf;; *.docx) echo application/vnd.openxmlformats-officedocument.wordprocessingml.document;; *.xlsx) echo application/vnd.openxmlformats-officedocument.spreadsheetml.sheet;; *.pptx) echo application/vnd.openxmlformats-officedocument.presentationml.presentation;; *) echo text/plain;; esac)" $1 > /dev/null 2>&1
    sleep 10; scroll 4; monkey_in $1 80 ;;
  *) launch $1; sleep 5; case " $CONTENT " in *" $1 "*) monkey_in $1 150 ;; *) scroll 2 ;; esac ;;
  esac
  input keyevent 3; sleep 1
}
round=0
while [ $(date +%s) -lt $end ]; do
  round=$((round + 1))
  apps=$(for a in $CONTENT $OTHER; do echo "$RANDOM $a"; done | sort -n | cut -d' ' -f2)
  for a in $apps; do
    [ $(date +%s) -lt $end ] || break
    guard; use $a
  done
  echo "$(date +%T) round $round: $(grep -E '^(pswpin|pswpout) ' /proc/vmstat | tr '\n' ' ') mm_stat $(cat /sys/block/zram0/mm_stat)"
done
