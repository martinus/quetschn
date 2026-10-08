# Tools

Everything around the codec that is not the harness in `bench/`: collecting pages, checking the
format, measuring in a kernel and on a phone, and drawing the results. Programs have a directory of
their own, scripts are at the top. The commands are in [docs/measuring.md](../docs/measuring.md).

| tool | what it does |
| --- | --- |
| [`collect/`](collect) | the page collectors: `quetschn-collect-resident` samples resident memory, `quetschn-import-raw` turns a `dd` of a zram device into a corpus, `quetschn-split-corpus` and `quetschn-sample-corpus` split it and draw samples |
| [`seqlz-ref/`](seqlz-ref) | two decoders written from [docs/format.md](../docs/format.md) alone, slow on purpose, in Python and in C. The tests and the fuzzers check `seqlz_decode()` against the C one, and `check.sh`, run by CI, checks the spec's table hashes and decodes pages `src/` compressed with the Python one |
| [`kernel-port/`](kernel-port) | `port.py` writes the codec into a Linux tree as the kernel would have it: `lib/seqlz/`, `include/linux/seqlz.h`, Kconfig, and a zram backend `seqlz` without the experiments of `zram-vm/`. `check.sh`, run by CI, builds that in a copy of a tree and runs checkpatch and kernel-doc on it |
| [`zram-vm/`](zram-vm) | `run.sh` builds a kernel with seqlz as a zram backend, boots it in a VM, and measures zram's reads and writes, or with `MODE=swap` the whole page fault |
| [`zram-phone/`](zram-phone) | zram's writes and reads in the Mi 9T's own Linux 4.14: `build.sh` makes a module of the codec for that kernel, with the phone's symbol CRCs (`crcs.py`, `patch_versions.py`), `run.sh` runs `zramphone` on each codec alone, in turns, logs the clocks and temperatures around every run and can fix the memory's clocks and stop Android, `summary.py` gives the mean and the range per codec |
| [`swap-fault/`](swap-fault) | the whole page fault on a running Linux as root, e.g. a rooted phone |
| [`phone-apps/`](phone-apps) | the app launch test on a rooted Android phone: 25 apps in turn, zram on each codec, a reboot before every run |
| [`android-emu/`](android-emu) | a zram dump from the Android 17 emulator, with 4 KiB or 16 KiB pages: its apps in turn while `hog` holds the RAM |
| [`swap-bursts/`](swap-bursts) | `quetschn-swap-bursts`: samples `pswpin`, groups the swap-ins into bursts, and gives `r`, the reads per write of the score |
| [`seqlz-bound/`](seqlz-bound) | per kind of field the bits a dump's pages take, next to their entropy, and layouts of the offset classes priced on real sequences |
| [`seqlz-branches/`](seqlz-branches) | which branches of the decoder mispredict on pages it sees once, as in a swap-in: `perf stat` per decode, and the mispredicted branches by source line from the CPU's branch records |
| [`seqlz-worst/`](seqlz-worst) | the worst case: `quetschn-seqlz-worst` counts the instructions of every page of a corpus and writes made-up pages that are slow, `cost_fuzz.c` searches for slower ones, also for `lz4`, `lzo-rle` and `zstd`. The slowest pages found are in [`seqlz-worst/pages/`](seqlz-worst/pages) |
| [`seqlz-viz/`](seqlz-viz) | builds [seqlz, bit by bit](../docs/seqlz-bit-by-bit.html) and [seqlz, compressed](../docs/seqlz-compressed.html), see its [README](seqlz-viz/README.md) |
| [`check-links.py`](check-links.py) | every relative link and `#anchor` in the Markdown files must resolve, run by CI |
| [`quick-bench.sh`](quick-bench.sh) | the fast benchmark for trying out a change: exact sizes on the whole corpus, times on a sample of 20 000 pages in 5 processes |
| [`bench-dict.sh`](bench-dict.sh) | trains a dictionary on one dump and measures every codec with and without it on another |
| [`plot-codecs.py`](plot-codecs.py) | memory against time, from the logs of `zram-vm/run.sh` |
| [`plot-swap-fault.py`](plot-swap-fault.py) | the same for the whole page fault, from `zram-vm/run.sh` with `MODE=swap` and from `swap-fault/` |
| [`plot-zstd-levels.py`](plot-zstd-levels.py) | `seqlz-fast-lit` against `zstd` at each level, from `zram-vm/run.sh` logs with `MODE=swap` and `zstd:<level>` codecs |
| [`plot-devices.py`](plot-devices.py) | memory against time on several CPUs, one row per CPU |
| [`plot-speed.py`](plot-speed.py) | the first chart of [docs/seqlz.md](../docs/seqlz.md#the-result-lz4s-time-zstds-size-almost): cycles in a hot loop, and write, cold read and memory in the kernel VM |

The plot scripts need `matplotlib` and the Noto Sans font.
