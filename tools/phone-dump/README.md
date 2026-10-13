# zram dumps of a phone, with the process of every page

`session.sh` reboots the rooted Mi 9T, uses its apps for a while with `workload.sh`, and then writes
the zram device to a file. `swapmap` reads every process's `pagemap` and lists the owner of each swap
slot, so each page of the dump gets its process and its mapping, e.g. `[anon:libc_malloc]` of
`com.google.android.youtube`. `quetschn-split-corpus` can then put whole apps on one side.

The workload: web pages in Chrome and in Mi Browser (random Wikipedia articles, news, shops, searches),
YouTube videos and searches, Google Maps at random places, the camera, documents in WPS Office, and
`monkey` in the apps with content. `urls-train.txt` and `urls-test.txt` have no site in common, so a
test dump has web pages that the training dumps did not see.

The kernel's lowmemorykiller has to be off. With MIUI's minfree it killed every app before its pages
reached zram, and the dump held only system services. `start.sh` switches it off and hides it, so that
lmkd kills by memory pressure, as on current phones (the same as `LMK=psi` of
[`phone-apps/`](../phone-apps)).

```sh
CC=~/opt/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang
$CC -O2 -static -o swapmap tools/phone-dump/swapmap.c
$CC -O2 -static -o hog tools/phone-apps/hog.c
# swapmap, hog, start.sh and workload.sh go to /data/local/tmp/qdump (as root), documents to
# /sdcard/Download/qdocs for WPS Office
tools/phone-dump/session.sh p2 95 22 urls-train.txt 1536    # name, minutes, seed, URLs, MiB for hog
build/quetschn-import-raw --in p2.raw --out p2
tools/phone-dump/attribute.py p2 p2.map1 p2.map2
```

A slot gets its owner only when `swapmap` saw the same owners before and after the `dd`, which was 87%
to 95% of the pages; the rest keeps the process name `?`. The phone swaps a lot in this workload, so
it says nothing about `r`, the reads per write of the score.

Don't put the Clock app back into `workload.sh`: the swipes in it set alarms, and an alarm rings at
full volume.
