#!/system/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# emuapps.sh <rounds> [b]: the apps of the Android 17 emulator image with Google APIs, round after round,
# as tools/phone-apps/swapbench.sh does on a phone, so that zram fills with the pages of apps. b is the
# second run: the apps in reverse order and other web pages in Chrome. Run as root (adb root), with hog
# from tools/phone-apps/ holding most of the RAM. Prints the swap counters and zram's mm_stat per round.
rounds=${1:-3}
APPS="com.android.chrome com.google.android.youtube com.google.android.apps.maps com.google.android.gm com.google.android.apps.photos com.google.android.calendar com.google.android.contacts com.google.android.dialer com.google.android.apps.messaging com.google.android.deskclock com.google.android.apps.docs com.android.camera2 com.android.settings com.google.android.documentsui com.google.android.apps.youtube.music com.google.android.googlequicksearchbox"
URLS="https://en.wikipedia.org/wiki/Linux_kernel https://www.bbc.com/news https://github.com/torvalds/linux https://www.theverge.com https://developer.android.com https://www.kernel.org"
if [ "$2" = b ]; then
  rev=""; for a in $APPS; do rev="$a $rev"; done; APPS=$rev
  URLS="https://en.wikipedia.org/wiki/Zram https://www.reuters.com https://news.ycombinator.com https://www.lwn.net https://www.mozilla.org https://www.python.org"
fi
svc power stayon true; input keyevent 224; wm dismiss-keyguard
device_config put activity_manager max_cached_processes 96
for s in 1 2 3 4 5; do cmd media_session volume --stream $s --set 0 > /dev/null 2>&1; done
r=1
while [ $r -le $rounds ]; do
  for a in $APPS; do
    # Android's cached process limit, which a flag sync after boot sets back to 32, as in swapbench.sh
    [ "$(device_config get activity_manager max_cached_processes)" = 96 ] || device_config put activity_manager max_cached_processes 96
    comp=$(cmd package resolve-activity --brief $a | tail -1)
    timeout 30 am start -W -n $comp > /dev/null 2>&1
    if [ $a = com.android.chrome ] && [ $r = 1 ]; then
      for u in $URLS; do
        am start -a android.intent.action.VIEW -d $u com.android.chrome > /dev/null 2>&1; sleep 5
      done
    fi
    sleep 4
    input swipe 540 1700 540 600 300; sleep 1; input swipe 540 1700 540 600 300; sleep 1
    input keyevent 3; sleep 1
  done
  echo "round $r: $(grep -E '^(pswpin|pswpout) ' /proc/vmstat | tr '\n' ' ') mm_stat $(cat /sys/block/zram0/mm_stat)"
  r=$((r + 1))
done
