#!/bin/bash
# run.sh: the app switching workload on a rooted Android phone over adb, zram0 on each algorithm in
# turn, a reboot before every run. The phone needs setup.sh, swapbench.sh, hog (hog.c built with the
# NDK's clang, --target=aarch64-linux-android30 -static) and the compressor modules in
# /data/local/tmp/quetschn, the lock screen off, and Magisk's su allowed for the shell.
#
#   ALGOS="lzo lz4 seqlz-fast"  the algorithms, each repetition starts at another one
#   REPS="1 2 3"                the repetitions
#   DISKS="algo:bytes ..."      an algorithm's own zram disksize, 2.5 GiB otherwise
#   HOG=1536                    MiB that hog locks, so the apps fit in less RAM
#   LMK=psi                     the kernel's lowmemorykiller off and hidden, lmkd kills by pressure
#   ROUNDS=4                    rounds over all apps
#   RESULTS=results             the directory for the results, one subdirectory per run
#
# python3 analyze.py results, python3 perlaunch.py results: the summary
OUT=${RESULTS:-results}; mkdir -p "$OUT"
read -r -a algos <<< "${ALGOS:-lzo lz4 seqlz-fast}"
n=${#algos[@]}
for rep in ${REPS:-1 2 3}; do for k in $(seq 0 $((n - 1))); do
  algo=${algos[$(( (k + (rep - 1) * (n + 1) / 2) % n ))]}
  disk=$(tr ' ' '\n' <<< "${DISKS:-}" | awk -F: -v a="$algo" '$1==a {print $2}')
  echo "=== rep $rep $algo $(date +%T) ${disk:+disksize $disk}"
  adb reboot; sleep 20; adb wait-for-device
  until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 5; done
  # the phone is busy for a while after boot
  sleep 90
  adb shell "su -mm -c 'sh /data/local/tmp/quetschn/setup.sh $algo ${HOG:-1536} ${LMK:--} $disk'" 2>&1 | tail -5
  adb shell "su -c 'cd /data/local/tmp/quetschn && rm -rf run && sh swapbench.sh run ${ROUNDS:-4} ${SET:-}'" 2>&1 | tail -1
  run="$OUT/$rep-$algo"
  rm -rf -- "${run:?}"; adb pull /data/local/tmp/quetschn/run "$run" > /dev/null
  echo "RESULT rep $rep $algo: $(awk '$3=="COLD" && $1>1' "$run/launches.txt" | wc -l) cold launches after round 1"
done; done
echo "=== all done $(date +%T)"
