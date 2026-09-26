#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# zram's read latency in a VM, with and without prefetching the compressed data and the destination
# before decompression (docs/explored-designs.md). Builds a kernel from a Linux tree with
# zram-prefetch.patch applied to a copy, and an initramfs whose /init (init.c) writes the pages of a
# corpus to /dev/zram0 and reads them back with O_DIRECT. The corpus pages go into the initramfs: it is
# written with mode 600 and deleted at the end, like the corpus it contains private data.

set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: [ALGOS=lz4,seqlz] [LLVM=1] tools/zram-vm/run.sh <linux tree> <corpus base>" >&2; exit 2; }
# LLVM=1 builds the kernel with clang, as Android does, instead of gcc
kmake=(make ${LLVM:+LLVM=$LLVM})
tree=$1
corpus=$2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
chmod 700 "$work"
trap 'rm -rf "$work"' EXIT

git -C "$tree" archive HEAD | tar -x -C "$work" --one-top-level=src
patch -d "$work/src" -p1 <"$here/zram-prefetch.patch"
# seqlz and bytelz as zram backends, with lz4's -O3
z="$work/src/drivers/block/zram"
cp "$here/backend_seqlz.c" "$here/backend_seqlz.h" "$here/backend_bytelz.c" "$here/backend_bytelz.h" \
    "$here/backend_seqlz_hc.c" "$here/backend_seqlz_hc.h" \
    "$here/../../explore/seqlz.c" "$here/../../explore/seqlz.h" "$here/../../explore/bytelz.c" \
    "$here/../../explore/bytelz.h" "$here/../../explore/page_lz.h" "$here/../../explore/seqlz_default_tables.c" \
    "$here/../../explore/seqlz_lit_sets.c" "$z/"
sed -i 's|#include "backend_842.h"|#include "backend_842.h"\n#include "backend_seqlz.h"\n#include "backend_bytelz.h"\n#include "backend_seqlz_hc.h"|; s|^\tNULL$|\t\&backend_seqlz,\n\t\&backend_seqlz_lit,\n\t\&backend_bytelz,\n\t\&backend_seqlz_hc,\n\t\&backend_seqlz_hc_lit,\n\tNULL|' "$z/zcomp.c"
printf 'zram-y += backend_seqlz.o seqlz.o seqlz_default_tables.o seqlz_lit_sets.o backend_bytelz.o bytelz.o backend_seqlz_hc.o\n' >>"$z/Makefile"
printf 'CFLAGS_seqlz.o += -O3\nCFLAGS_bytelz.o += -O3\n' >>"$z/Makefile"
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
taskset -c 2 qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G -kernel "$work/build/arch/x86/boot/bzImage" \
    -initrd "$work/initramfs.cpio" -append "console=ttyS0 quiet panic=-1 zram.num_devices=8 quetschn.algos=${ALGOS:-lz4}" \
    -nographic -no-reboot |
    grep -a RESULT
