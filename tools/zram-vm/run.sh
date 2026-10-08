#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# zram's read latency in a VM, with and without prefetching the compressed data and the destination
# before decompression (docs/explored-designs.md). Builds a kernel from a Linux tree with
# zram-prefetch.patch applied to a copy (zram-prefetch-sg.patch where zram reads the compressed data
# through a scatterlist, mm-unstable of October 2026), and an initramfs whose /init (init.c) writes the pages of a
# corpus to /dev/zram0 and reads them back with O_DIRECT. The corpus pages go into the initramfs: it is
# written with mode 600 and deleted at the end, like the corpus it contains private data. MODE=swap
# measures zram as swap instead, the whole page fault of a swap-out and a swap-in (init.c). KARGS adds
# to the kernel command line, e.g. KARGS=zram.zram_prefetch=8 for the backends' own prefetch. BOOTS=n
# boots the kernel n times, the order of ALGOS rotated by one per boot, each boot's lines after a line
# "BOOT k of n", and prints the range of every mean over the boots at the end; one boot can be off by
# 18%. quetschn-score takes the mean over the boots.

set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: [ALGOS=lz4,seqlz-lit] [LLVM=1] [MODE=swap] [BOOTS=3] tools/zram-vm/run.sh <linux tree> <corpus base>" >&2; exit 2; }
# ALGOS: zram's names of the backends, a level after a colon, e.g. zstd:-1 or zstd:9
# LLVM=1 builds the kernel with clang, as Android does, instead of gcc
kmake=(make ${LLVM:+LLVM=$LLVM})
tree=$1
corpus=$2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
chmod 700 "$work"
trap 'rm -rf "$work"' EXIT

git -C "$tree" archive HEAD | tar -x -C "$work" --one-top-level=src
# trees that read the compressed data through a scatterlist (mm-unstable of October 2026) need the
# experiments where it is mapped, zram-prefetch-sg.patch
if grep -q zs_obj_read_sg_begin "$work/src/drivers/block/zram/zram_drv.c"; then
    patch -d "$work/src" -p1 <"$here/zram-prefetch-sg.patch"
else
    patch -d "$work/src" -p1 <"$here/zram-prefetch.patch"
fi
# seqlz as zram backends, seqlz (raw literals), seqlz-lit and seqlz-hc (levels 3 and 4), with lz4's -O3
z="$work/src/drivers/block/zram"
cp "$here/backend_seqlz.c" "$here/backend_seqlz.h" "$here/../../src/seqlz.c" "$here/../../src/seqlz.h" \
    "$here/../../src/page_lz.h" "$here/../../src/seqlz_compat.h" "$here/../../src/seqlz_default_tables.c" \
    "$here/../../src/seqlz_default_tables_4k.inc" "$here/../../src/seqlz_lit_sets.c" \
    "$here/../../src/seqlz_lit_sets_4k.inc" "$z/"
sed -i 's|#include "backend_842.h"|#include "backend_842.h"\n#include "backend_seqlz.h"|; s|^\tNULL$|\t\&backend_seqlz,\n\t\&backend_seqlz_lit,\n\t\&backend_seqlz_hc,\n\tNULL|' "$z/zcomp.c"
printf 'zram-y += backend_seqlz.o seqlz.o seqlz_default_tables.o seqlz_lit_sets.o\n' >>"$z/Makefile"
printf 'CFLAGS_seqlz.o += -O3\n' >>"$z/Makefile"
# zram's contexts split into compression and decompression, see backend_seqlz.c
if grep -q 'struct zcomp_cstrm' "$z/zcomp.h"; then
    printf 'CFLAGS_backend_seqlz.o += -DZCOMP_RW_SPLIT\n' >>"$z/Makefile"
    echo "KERNEL zcomp with separate compression and decompression contexts"
fi
"${kmake[@]}" -C "$work/src" O="$work/build" defconfig >/dev/null
"$work/src/scripts/config" --file "$work/build/.config" --enable ZRAM --enable ZSMALLOC --enable ZRAM_BACKEND_LZ4 --enable ZRAM_BACKEND_LZO --enable ZRAM_BACKEND_ZSTD --enable ZRAM_BACKEND_LZ4HC \
    --enable DEVTMPFS --enable BLK_DEV_INITRD --enable ZRAM_MULTI_COMP --enable ZRAM_TRACK_ENTRY_ACTIME
"${kmake[@]}" -C "$work/src" O="$work/build" olddefconfig >/dev/null
"${kmake[@]}" -C "$work/src" O="$work/build" -j"$(nproc)" bzImage >/dev/null
grep -h "^CONFIG_CC_VERSION_TEXT" "$work/build/.config" | sed 's/^/KERNEL /'

gcc -static -O2 -o "$work/init" "$here/init.c"
mkdir -p "$work/root/dev" "$work/root/sys" "$work/root/proc"
cp "$work/init" "$work/root/init"
cp "$corpus.pages" "$work/root/pages"
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null) >"$work/initramfs.cpio"
chmod 600 "$work/initramfs.cpio"

# CPU 2, as the other benchmarks; set a fixed frequency yourself
boots=${BOOTS:-1}
algos=${ALGOS:-lz4}
for ((k = 1; k <= boots; k++)); do
    [[ $boots -eq 1 ]] || echo "BOOT $k of $boots: ALGOS=$algos"
    taskset -c 2 qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G -kernel "$work/build/arch/x86/boot/bzImage" \
        -initrd "$work/initramfs.cpio" -append "console=ttyS0 quiet panic=-1 zram.num_devices=8 quetschn.algos=$algos quetschn.mode=${MODE:-read} zswap.enabled=0 ${KARGS:-}" \
        -nographic -no-reboot |
        grep -a RESULT | tee -a "$work/boots.log"
    # the first codec goes last, so that each one is in every place once in as many boots as codecs
    if [[ $algos == *,* ]]; then algos=${algos#*,},${algos%%,*}; fi
done
[[ $boots -eq 1 ]] || python3 - "$work/boots.log" <<'EOF'
import re, sys
# every "mean <x> ns" of a codec and what was measured, over the boots: the range, in us
seen = {}
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"RESULT (\S+)\s+(.*?)\s*(?:n\s+\d+)?:.* mean (\d+) ns", line)
    if m:
        seen.setdefault((m[1], re.sub(r"\s+", " ", m[2])), []).append(int(m[3]) / 1000)
for (codec, what), v in seen.items():
    print(f"RANGE {codec:15s} {what:40s} {min(v):8.2f} to {max(v):8.2f} us, {max(v) - min(v):.2f} over {len(v)} boots")
EOF
