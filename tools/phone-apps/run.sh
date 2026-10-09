#!/bin/bash
# run.sh: the app switching workload on a rooted Android phone over adb, zram0 on each algorithm in
# turn, a reboot before every run. The phone needs setup.sh, swapbench.sh, hog (hog.c built with the
# NDK's clang, --target=aarch64-linux-android30 -static) and the compressor modules in
# /data/local/tmp/quetschn, the lock screen off, Magisk's su allowed for the shell, and the Play Store's
# auto update off, so that every run has the same app versions (each run writes them to versions.txt).
#
#   ALGOS="lzo lz4 seqlz-fast"  the algorithms, each repetition starts at another one
#   REPS="1 2 3"                the repetitions
#   DISKS="algo:bytes ..."      an algorithm's own zram disksize, 2.5 GiB otherwise
#   HOG=1536                    MiB that hog locks, so the apps fit in less RAM
#   LMK=psi                     the kernel's lowmemorykiller off and hidden, lmkd kills by pressure
#   ROUNDS=4                    rounds over all apps
#   RESULTS=results             the directory for the results, one subdirectory per run
#   END=05:55                   start no run that would end after this time (a run takes about 23 min)
#
# A run where Android set its limit on cached processes back after round 1, or where a launch hung,
# is spoiled: both kill or restart apps for reasons other than the codec. It is kept as
# <rep>-<algo>-spoiled and run once more.
#
# python3 analyze.py results, python3 perlaunch.py results: the summary
OUT=${RESULTS:-results}; mkdir -p "$OUT"
read -r -a algos <<< "${ALGOS:-lzo lz4 seqlz-fast}"
n=${#algos[@]}
# one run of algo on a freshly booted phone, into $OUT/$rep-$algo
one() {
  adb reboot; sleep 20; adb wait-for-device
  until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 5; done
  # the phone is busy for a while after boot
  sleep 90
  adb shell "su -mm -c 'sh /data/local/tmp/quetschn/setup.sh $algo ${HOG:-1536} ${LMK:--} $disk'" 2>&1 | tail -6
  adb shell "su -c 'cd /data/local/tmp/quetschn && rm -rf run && sh swapbench.sh run ${ROUNDS:-4} ${SET:-}'" 2>&1 | tail -1
  rm -rf -- "${run:?}"; adb pull /data/local/tmp/quetschn/run "$run" > /dev/null
}
spoiled() { awk '($NF == "reset" && $1 > 1) || $NF == "hung"' "$1/limit.txt" 2>/dev/null | grep -q .; }
end=$([ -n "${END:-}" ] && date -d "$END" +%s || echo 0)
# before midnight, an END after midnight is tomorrow
[ "$end" -gt 0 ] && [ "$end" -lt "$(date +%s)" ] && end=$((end + 86400))
for rep in ${REPS:-1 2 3}; do for k in $(seq 0 $((n - 1))); do
  algo=${algos[$(( (k + (rep - 1) * (n + 1) / 2) % n ))]}
  disk=$(tr ' ' '\n' <<< "${DISKS:-}" | awk -F: -v a="$algo" '$1==a {print $2}')
  run="$OUT/$rep-$algo"
  for try in 1 2; do
    if [ "$end" -gt 0 ] && [ $(( $(date +%s) + 25 * 60 )) -gt "$end" ]; then
      echo "=== END reached $(date +%T), not starting rep $rep $algo"; break 3
    fi
    echo "=== rep $rep $algo $(date +%T) ${disk:+disksize $disk}$([ $try = 2 ] && echo ' again')"
    one
    echo "RESULT rep $rep $algo: $(awk '$3=="COLD" && $1>1' "$run/launches.txt" | wc -l) cold launches after round 1"
    spoiled "$run" || break
    # a second spoiled run stays where it is, analyze.py does not count it
    [ $try = 2 ] && break
    echo "SPOILED rep $rep $algo: $(awk '($NF == "reset" && $1 > 1) || $NF == "hung"' "$run/limit.txt" | tr '\n' ';')"
    rm -rf -- "$run-spoiled"; mv "$run" "$run-spoiled"
  done
done; done
echo "=== all done $(date +%T)"
