#!/system/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# start.sh <minutes> <seed> <hog MiB>: the phone's side of session.sh, as root in /data/local/tmp/qdump:
# settings for the workload, hog, then workload.sh in the background; it touches done when it ends.
# The kernel's lowmemorykiller off and hidden, so that lmkd kills by memory pressure (PSI) as on
# current phones: with it, MIUI's minfree killed every app before its pages reached zram. Needs su -mm.
cd /data/local/tmp/qdump || exit 1
echo 0 > /sys/module/lowmemorykiller/parameters/enable_lmk
mount -t tmpfs -o ro,size=4k tmpfs /sys/module/lowmemorykiller/parameters
setprop ctl.restart lmkd; sleep 3
echo "lmk params: $(ls /sys/module/lowmemorykiller/parameters | wc -l), lmkd $(getprop init.svc.lmkd)"
svc power stayon usb; input keyevent 224; wm dismiss-keyguard
device_config put activity_manager max_cached_processes 96
for s in 1 2 3 4 5; do cmd media_session volume --stream $s --set 0 > /dev/null 2>&1; done
pm grant cn.wps.xiaomi.abroad.lite android.permission.READ_EXTERNAL_STORAGE
appops set cn.wps.xiaomi.abroad.lite MANAGE_EXTERNAL_STORAGE allow
rm -f done
setsid sh -c "echo -1000 > /proc/self/oom_score_adj; exec ./hog $3 > hog.log 2>&1 < /dev/null" &
sleep 5
setsid sh -c "sh workload.sh $1 $2 urls.txt > workload.log 2>&1; touch done" < /dev/null > /dev/null 2>&1 &
