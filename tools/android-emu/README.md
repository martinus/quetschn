# A zram dump from the Android emulator

The Android 17 emulator images exist with 4 KiB and with 16 KiB pages, and no phone here has 16 KiB
pages. `emuapps.sh` runs the apps of the image, round after round, while `hog` from
[`phone-apps/`](../phone-apps) holds most of the RAM, so that zram fills with the pages of apps. Then a
`dd` of the zram device is a corpus, as on a phone. The image has no account signed in, so the pages
hold no one's data, but they are a corpus like any other: keep them in `~/quetschn-corpus` and out of
the repository.

The images are `system-images;android-37.0;google_apis_ps16k;x86_64` and
`system-images;android-37.0;google_apis;x86_64`, the device `pixel_8` with 4 GB of RAM and 4 cores.
For the 16 KiB image `hog` needs its segments aligned to 16 KiB, or it crashes on start:

```sh
avdmanager create avd -n q16k -k "system-images;android-37.0;google_apis_ps16k;x86_64" -d pixel_8
emulator -avd q16k -no-window -no-audio -no-boot-anim -no-snapshot -wipe-data -gpu swiftshader_indirect -port 5554 &
gcc -O2 -static -Wl,-z,max-page-size=16384 -o hog tools/phone-apps/hog.c
adb -s emulator-5554 root
adb -s emulator-5554 push hog tools/android-emu/emuapps.sh /data/local/tmp/
adb -s emulator-5554 shell 'cd /data/local/tmp; setsid sh -c "echo -1000 > /proc/self/oom_score_adj; exec ./hog 2048 > hog.log 2>&1 < /dev/null" &'
adb -s emulator-5554 shell 'cd /data/local/tmp; sh emuapps.sh 3'      # the second run, after a reboot: sh emuapps.sh 3 b
adb -s emulator-5554 exec-out 'dd if=/dev/block/zram0 bs=1048576 2>/dev/null' |
    dd of=$HOME/quetschn-corpus/emu-a17-16k-a.raw bs=16384 iflag=fullblock conv=sparse
./build/quetschn-import-raw --in ~/quetschn-corpus/emu-a17-16k-a.raw --out ~/quetschn-corpus/emu-a17-16k-a --page-size 16384
```

`hog` started as a child of the `adb shell` dies when the shell ends, hence `setsid`, and without the
`oom_score_adj` lmkd kills it first. The system keeps swapping while the `dd` runs, so a dump has more
pages than `mm_stat` showed right before it. Each run takes about 15 minutes. A second run after a
reboot with `-wipe-data`, `emuapps.sh 3 b`, gives the dump to measure on, see
[docs/measuring.md](../../docs/measuring.md#pages-to-measure-on).
