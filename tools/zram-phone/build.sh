#!/bin/bash
# build.sh <name> <src dir> <out dir>: the codec in <src dir> (a copy of src/) as a module for the Mi 9T's
# Linux 4.14, <out dir>/quetschn_<name>.ko, registering the crypto compressors seqlz-<name> and
# seqlz-<name>-lit for zram. Built as the phone's kernel is, with NDK r21e's clang 9, the codec with -O3
# as lib/lz4, every function aligned to 64 bytes so that a change in one does not move the others. The
# module's symbol versions get the phone's CRCs (patch_versions.py), else 4.14 refuses it.
#
# Needs, in $MI9T_KERNEL (default ~/opt/mi9t-kernel):
#   src/            phoenix-r-oss of MiCode/Xiaomi_Kernel_OpenSource, 4.14.180 for the same SoC family
#   out/            that tree configured with the phone's /proc/config.gz and its version string, after
#                   make modules_prepare, so out/Module.symvers exists
#   hoststub/       headers the tree's host tools want and the machine lacks (openssl/)
#   phone-crcs.txt  the CRCs of the phone's own modules, from crcs.py
# and NDK r21e in $NDK_R21E (default ~/opt/android-ndk-r21e).
set -euo pipefail
[[ $# -eq 3 ]] || { sed -n '2,15p' "$0" >&2; exit 2; }
name=$1; src=$(realpath "$2"); out=$(realpath -m "$3")
here=$(cd "$(dirname "$0")" && pwd)
K=${MI9T_KERNEL:-$HOME/opt/mi9t-kernel}
T=${NDK_R21E:-$HOME/opt/android-ndk-r21e}/toolchains
rm -rf "$out"; mkdir -p "$out"
cp "$src"/seqlz.c "$src"/seqlz.h "$src"/page_lz.h "$src"/seqlz_compat.h "$src"/seqlz_default_tables.c \
    "$src"/seqlz_default_tables_*.inc "$src"/seqlz_lit_sets.c "$src"/seqlz_lit_sets_*.inc "$out"/
sed -e "s/\"seqlz-fast-lit\"/\"seqlz-$name-lit\"/; s/\"seqlz-fast\"/\"seqlz-$name\"/" \
    -e "s/\"seqlz-fast-lit-generic\"/\"seqlz-$name-lit-generic\"/; s/\"seqlz-fast-generic\"/\"seqlz-$name-generic\"/" \
    "$here/glue.c" >"$out/glue.c"
printf 'obj-m += quetschn_%s.o\nquetschn_%s-y := glue.o seqlz.o seqlz_default_tables.o seqlz_lit_sets.o\nCFLAGS_seqlz.o += -O3\nccflags-y += -falign-functions=64\n' \
    "$name" "$name" >"$out/Kbuild"
export PATH=$T/llvm/prebuilt/linux-x86_64/bin:$T/aarch64-linux-android-4.9/prebuilt/linux-x86_64/bin:$PATH
make -C "$K/src" O="$K/out" ARCH=arm64 CC=clang CLANG_TRIPLE=aarch64-linux-gnu- CROSS_COMPILE=aarch64-linux-android- \
    HOSTCFLAGS=-I"$K/hoststub" M="$out" modules 2>&1 | grep -E ' error|warning:' || true
python3 "$here/patch_versions.py" "$K/phone-crcs.txt" "$K/out/Module.symvers" "$out/quetschn_$name.ko" >/dev/null
ls -l "$out/quetschn_$name.ko"
