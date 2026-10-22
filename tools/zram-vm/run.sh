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
# 18%. quetschn-score takes the mean over the boots. VARIANTS="name=dir[:cflags] ..." adds more builds of
# seqlz as backends <name>-1 to <name>-3, from <dir>/src, e.g. another worktree, with cflags after its
# -O3, e.g. VARIANTS="o2=.:-O2 le16=../other": variants compared within one boot, the codecs taking turns.
# PORT=1 builds seqlz as tools/kernel-port/port.py writes it into the tree, the kernel's own copy, with
# its backend seqlz and the level as zram's parameter, e.g. ALGOS=lz4,seqlz:2.
# ALGOS with wkdm adds WKdm, the page compressor of Wilson and Kaplan, Apple's C version downloaded from
# xnu, see backend_wkdm.c.
# The kernel gets ../kernel-port/x86-prefetcht0.patch, which the series has: without it a clang build
# drops prefetch() on x86-64.

set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: [ALGOS=lz4,seqlz-2] [LLVM=1] [MODE=swap] [BOOTS=3] [VARIANTS=name=dir[:cflags]] tools/zram-vm/run.sh <linux tree> <corpus base>" >&2; exit 2; }
# ALGOS: zram's names of the backends, a level after a colon, e.g. zstd:-1 or zstd:9; at most 8, one zram
# device each (init.c's MAX_ALGOS, zram.num_devices): init.c dropped the rest without a word
n_algos=$(tr ',' '\n' <<<"${ALGOS:-lz4}" | wc -l)
[[ $n_algos -le 8 ]] || { echo "ALGOS has $n_algos codecs, at most 8 fit into one boot" >&2; exit 2; }
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
# unless the tree has it already
patch -d "$work/src" -p1 -R -s -f --dry-run <"$here/../kernel-port/x86-prefetcht0.patch" >/dev/null ||
    patch -d "$work/src" -p1 -s <"$here/../kernel-port/x86-prefetcht0.patch"
if [[ -n ${PORT:-} ]]; then
    # the kernel's copy as port.py writes it, its backend seqlz with the levels as zram's parameter
    [[ -z ${VARIANTS:-} ]] || { echo "PORT and VARIANTS do not go together" >&2; exit 2; }
    python3 "$here/../kernel-port/port.py" "$work/src" >/dev/null
    echo "KERNEL seqlz as tools/kernel-port/port.py writes it"
else
    # seqlz as zram backends, seqlz-1 to seqlz-3, one per level, with lz4's -O3
    z="$work/src/drivers/block/zram"
    levels="1 2 3"
    # the codec's files, whether a src/ has it in one seqlz.c or split
    codec_objs() { (cd "$1" && ls seqlz*.c | sed 's/\.c$/.o/' | tr '\n' ' '); }
    cp "$here/backend_seqlz.c" "$here/backend_seqlz.h" "$here/../../src/"seqlz*.c "$here/../../src/"*.h \
        "$here/../../src/"*_4k.inc "$z/"
    sed -i 's|#include "backend_842.h"|#include "backend_842.h"\n#include "backend_seqlz.h"|' "$z/zcomp.c"
    for l in $levels; do
        sed -i "s|^\tNULL$|\t\&backend_seqlz_$l,\n\tNULL|" "$z/zcomp.c"
    done
    objs=$(codec_objs "$here/../../src")
    printf 'zram-y += backend_seqlz.o %s\n' "$objs" >>"$z/Makefile"
    for o in $objs; do
        printf 'CFLAGS_%s += -O3\n' "$o" >>"$z/Makefile"
    done
    for v in ${VARIANTS:-}; do
        name=${v%%=*} rest=${v#*=} flags=
        dir=${rest%%:*}
        [[ $rest == *:* ]] && flags=${rest#*:}
        d="$z/v_$name"
        mkdir -p "$d"
        # the variant's own backend, which calls its src/: the codec's interface changes between versions
        bdir=$dir/tools/zram-vm
        [[ -f $bdir/backend_seqlz.c ]] || bdir=$here
        cp "$bdir/backend_seqlz.c" "$bdir/backend_seqlz.h" "$d/"
        cp "$dir/src/"seqlz*.c "$dir/src/"*.h "$dir/src/"*_4k.inc "$d/"
        # backend_seqlz.c passes a level; a src/ from before the levels took a bool there, and would
        # build and code the literals at every level
        grep -q "define SEQLZ_LEVELS" "$d/seqlz.h" || { echo "variant $name: $dir/src has no levels, it is older than this run.sh" >&2; exit 1; }
        sed -i "s/^#define SZ_NAME \"seqlz-\"$/#define SZ_NAME \"$name-\"/" "$d/backend_seqlz.c"
        grep -q "^#define SZ_NAME \"$name-\"$" "$d/backend_seqlz.c" || { echo "variant $name: no backend name" >&2; exit 1; }
        # the global symbols of this build, renamed so that the builds do not clash
        # (the names of older versions too: seqlz_tables_size and seqlz_default_own before 22 Oct 2026)
        for s in seqlz_compress seqlz_decode seqlz_tables_init seqlz_tables_size seqlz_tables seqlz_all_symbols \
            seqlz_find seqlz_encode seqlz_default_lengths seqlz_default_own seqlz_lit_sets seqlz_own_lengths \
            $(printf 'backend_seqlz_%s ' $levels); do
            echo "#define $s ${s}_v_$name"
        done >"$d/rename.h"
        objs="v_$name/backend_seqlz.o $(codec_objs "$d" | sed "s#\([^ ][^ ]*\)#v_$name/\1#g")"
        printf 'zram-y += %s\n' "$objs" >>"$z/Makefile"
        for o in $objs; do
            printf 'CFLAGS_%s += -include $(src)/v_%s/rename.h -I$(src)\n' "$o" "$name" >>"$z/Makefile"
        done
        for o in $(codec_objs "$d"); do
            printf 'CFLAGS_v_%s/%s += -O3 %s\n' "$name" "$o" "$flags" >>"$z/Makefile"
        done
        for l in $levels; do
            sed -i "s|^#include \"backend_seqlz.h\"$|#include \"backend_seqlz.h\"\nextern const struct zcomp_ops backend_seqlz_${l}_v_$name;|; s|^\tNULL$|\t\&backend_seqlz_${l}_v_$name,\n\tNULL|" "$z/zcomp.c"
        done
        echo "VARIANT $name from $dir/src${flags:+, $flags}"
    done
    # zram's contexts split into compression and decompression, see backend_seqlz.c
    if grep -q 'struct zcomp_cstrm' "$z/zcomp.h"; then
        printf 'CFLAGS_backend_seqlz.o += -DZCOMP_RW_SPLIT\n' >>"$z/Makefile"
        for v in ${VARIANTS:-}; do
            printf 'CFLAGS_v_%s/backend_seqlz.o += -DZCOMP_RW_SPLIT\n' "${v%%=*}" >>"$z/Makefile"
        done
        echo "KERNEL zcomp with separate compression and decompression contexts"
    fi
fi
# WKdm, the page compressor of Wilson and Kaplan, as backend wkdm when ALGOS names it: Apple's C version
# from xnu (APSL 2.0), downloaded and checked here, never in this repository; see backend_wkdm.c
if [[ ,${ALGOS:-}, == *,wkdm[,:]* ]]; then
    z="$work/src/drivers/block/zram"
    xnu=https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-2050.48.11
    while read -r sum path; do
        curl -sSfL "$xnu/$path" -o "$z/${path##*/}"
        echo "$sum  $z/${path##*/}" | sha256sum -c --quiet
    done <<'EOF'
2bb2ac0b3e40042330f955c08a8854670ca6d91d426ef01badbf87ebd7712319 libkern/libkern/WKdm.h
1bfc569fc2a44884d2f6b8e55bd6b464767ef14df5353d90f5376876b34bbdff libkern/kxld/WKdmCompress.c
3cd96ce8394229db2e33740f26c59f7cdf53ee6c4f8244aac16827d3f75af5c1 libkern/kxld/WKdmDecompress.c
EOF
    # the work arrays off the stack, 7200 bytes each in the compressor and the decompressor
    sed -i 's/^\([[:space:]]*\)\(WK_word temp[A-Za-z]*Array\[\)/\1static \2/' "$z/WKdmCompress.c" "$z/WKdmDecompress.c"
    [[ $(grep -c 'static WK_word temp' "$z/WKdmCompress.c" "$z/WKdmDecompress.c" | grep -c ':3$') -eq 2 ]] ||
        { echo "WKdm: work arrays not found" >&2; exit 1; }
    # sys/cdefs.h gave __unused
    sed -i 's/^#include <sys\/cdefs.h>$/#define __unused __maybe_unused/' "$z/WKdmDecompress.c"
    cp "$here/backend_wkdm.c" "$here/backend_wkdm.h" "$z/"
    sed -i 's|#include "backend_842.h"|#include "backend_842.h"\n#include "backend_wkdm.h"|; s|^\tNULL$|\t\&backend_wkdm,\n\tNULL|' "$z/zcomp.c"
    printf 'zram-y += backend_wkdm.o WKdmCompress.o WKdmDecompress.o\n' >>"$z/Makefile"
    printf 'CFLAGS_%s += -O3 -Wno-missing-prototypes\n' WKdmCompress.o WKdmDecompress.o >>"$z/Makefile"
    grep -q 'struct zcomp_cstrm' "$z/zcomp.h" && printf 'CFLAGS_backend_wkdm.o += -DZCOMP_RW_SPLIT\n' >>"$z/Makefile"
    echo "KERNEL wkdm from xnu-2050.48.11, libkern/kxld"
fi
"${kmake[@]}" -C "$work/src" O="$work/build" defconfig >/dev/null
"$work/src/scripts/config" --file "$work/build/.config" --enable ZRAM --enable ZSMALLOC --enable ZRAM_BACKEND_LZ4 --enable ZRAM_BACKEND_LZO --enable ZRAM_BACKEND_ZSTD --enable ZRAM_BACKEND_LZ4HC \
    ${PORT:+--enable ZRAM_BACKEND_SEQLZ --enable SEQLZ} \
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
