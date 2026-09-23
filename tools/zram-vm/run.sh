#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# zram's read latency in a VM, with and without prefetching the compressed data and the destination
# before decompression (docs/explored-designs.md). Builds a kernel from a Linux tree with
# zram-prefetch.patch applied to a copy, and an initramfs whose /init (init.c) writes the pages of a
# corpus to /dev/zram0 and reads them back with O_DIRECT. The corpus pages go into the initramfs: it is
# written with mode 600 and deleted at the end, like the corpus it contains private data.

set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: tools/zram-vm/run.sh <linux tree> <corpus base>" >&2; exit 2; }
tree=$1
corpus=$2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
chmod 700 "$work"
trap 'rm -rf "$work"' EXIT

git -C "$tree" archive HEAD | tar -x -C "$work" --one-top-level=src
patch -d "$work/src" -p1 <"$here/zram-prefetch.patch"
make -C "$work/src" O="$work/build" defconfig >/dev/null
"$work/src/scripts/config" --file "$work/build/.config" --enable ZRAM --enable ZSMALLOC --enable ZRAM_BACKEND_LZ4 \
    --enable DEVTMPFS --enable BLK_DEV_INITRD
make -C "$work/src" O="$work/build" olddefconfig >/dev/null
make -C "$work/src" O="$work/build" -j"$(nproc)" bzImage >/dev/null

gcc -static -O2 -o "$work/init" "$here/init.c"
mkdir -p "$work/root/dev" "$work/root/sys" "$work/root/proc"
cp "$work/init" "$work/root/init"
cp "$corpus.pages" "$work/root/pages"
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null) >"$work/initramfs.cpio"
chmod 600 "$work/initramfs.cpio"

# CPU 2, as the other benchmarks; set a fixed frequency yourself
taskset -c 2 qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G -kernel "$work/build/arch/x86/boot/bzImage" \
    -initrd "$work/initramfs.cpio" -append "console=ttyS0 quiet panic=-1" -nographic -no-reboot |
    grep -a RESULT
