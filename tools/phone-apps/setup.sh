#!/system/bin/sh
# setup.sh <algo> <hog MiB> [nolmk|psi|-] [disksize bytes]: zram0 with algo as the only swap, the modules loaded, the hog
# started; nolmk switches the kernel's lowmemorykiller off until the next boot
algo=$1; hog=${2:-1536}; disk=${4:-2684354560}
[ "$3" = nolmk ] && echo 0 > /sys/module/lowmemorykiller/parameters/enable_lmk
# psi: the kernel's lowmemorykiller off and hidden, so that lmkd, restarted, kills by memory pressure
# as on current phones; needs su -mm for the mount
if [ "$3" = psi ]; then
  echo 0 > /sys/module/lowmemorykiller/parameters/enable_lmk
  mount -t tmpfs -o ro,size=4k tmpfs /sys/module/lowmemorykiller/parameters
  setprop ctl.restart lmkd; sleep 3
  echo "lmk params: $(ls /sys/module/lowmemorykiller/parameters | wc -l), lmkd $(getprop init.svc.lmkd)"
fi
cd /data/local/tmp/quetschn || exit 1
svc power stayon usb; input keyevent 224; wm dismiss-keyguard
# Android's limit on cached processes, 16 here, kills by count and not by memory; device_config does
# not survive a reboot
device_config put activity_manager max_cached_processes 96
# silent: the media apps of the workload start playing on their own
for s in 1 2 3 4 5; do cmd media_session volume --stream $s --set 0 > /dev/null 2>&1; done
insmod lz4_compress.ko 2>/dev/null; insmod lz4.ko 2>/dev/null; insmod quetschn_crypto.ko 2>/dev/null
insmod xxhash.ko 2>/dev/null; insmod zstd_compress.ko 2>/dev/null; insmod zstd_decompress.ko 2>/dev/null; insmod zstd.ko 2>/dev/null
swapoff /dev/block/zram0 || exit 1
echo 1 > /sys/block/zram0/reset
echo $algo > /sys/block/zram0/comp_algorithm || exit 1
echo $disk > /sys/block/zram0/disksize
mkswap /dev/block/zram0 > /dev/null && swapon /dev/block/zram0 || exit 1
cat /sys/block/zram0/comp_algorithm; cat /proc/swaps
nohup ./hog $hog > hog.log 2>&1 &
sleep 5; p=$(pidof hog); echo -1000 > /proc/$p/oom_score_adj; cat hog.log
sync; echo 3 > /proc/sys/vm/drop_caches; sleep 10
dumpsys activity settings | grep -E "CUR_MAX_CACHED|CUR_MAX_EMPTY" | tr -s " " | tr "\n" " "; echo
