#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# run.sh <linux tree>: where ../kernel-port/x86-prefetcht0.patch helps. Builds the tree's HEAD twice,
# without the patch (base) and with it (patch), reversing it where HEAD has it, x86-64 defconfig with
# null_blk, with clang (CC=gcc: with gcc, which emits prefetcht0 without the patch too, so base and patch
# are the same code, a control). Boots both BOOTS times (default 4) in turns, base first in odd boots,
# in a VM with one CPU pinned to CPU (default 2), and its /init, init.c, times system calls whose paths
# call prefetch(). Prints the header (kernel, compiler, the CPU's clock), each boot's results, and per
# benchmark the median over the boots of each kernel, the range over the boots, and the difference.
# Fix the clock first, sudo tools/pc-bench.sh set; nothing else may run meanwhile. Needs about 4 GB in
# $TMPDIR.
set -euo pipefail
[[ $# -eq 1 ]] || { sed -n '4,13p' "$0" >&2; exit 2; }
tree=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
patchfile=$here/../kernel-port/x86-prefetcht0.patch
cc=${CC:-clang}
cpu=${CPU:-2}
boots=${BOOTS:-4}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
kmake=(make)
[[ $cc == clang ]] && kmake=(make LLVM=1)
[[ $cc == clang || $cc == gcc ]] || { echo "CC: clang or gcc" >&2; exit 2; }

echo "HEADER tree $(git -C "$tree" log -1 --format='%h %s' | cut -c1-80)"
echo "HEADER compiler $($cc --version | head -1)"
echo "HEADER $("$here/../pc-bench.sh" show "$cpu" | tr '\n' ' ')"

for v in base patch; do
    src=$work/src-$v
    mkdir -p "$src"
    git -C "$tree" archive HEAD | tar -x -C "$src"
    has=0
    patch -d "$src" -p1 -R -s -f --dry-run <"$patchfile" >/dev/null 2>&1 && has=1
    if [[ $v == base && $has == 1 ]]; then
        patch -d "$src" -p1 -R -s <"$patchfile"
    elif [[ $v == patch && $has == 0 ]]; then
        patch -d "$src" -p1 -s <"$patchfile"
    fi
    o=$work/build-$v
    "${kmake[@]}" -C "$src" O="$o" defconfig >/dev/null
    "$src/scripts/config" --file "$o/.config" --enable BLK_DEV_NULL_BLK --enable IO_URING
    "${kmake[@]}" -C "$src" O="$o" olddefconfig >/dev/null
    "${kmake[@]}" -C "$src" O="$o" -j"$(nproc)" bzImage >"$o.log" 2>&1 || { tail -20 "$o.log"; exit 1; }
    echo "HEADER $v: $(objdump -d --no-show-raw-insn "$o/vmlinux" | grep -c prefetcht0) prefetcht0 in vmlinux"
    rm -rf "$src"
done

gcc -static -O2 -Wall -o "$work/init" "$here/init.c"
mkdir -p "$work/root/dev" "$work/root/proc" "$work/root/tmp"
cp "$work/init" "$work/root/init"
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null) >"$work/initramfs.cpio"

# null_blk: one device, a poll queue for io_uring's IOPOLL, completions right away
args="console=ttyS0 quiet panic=-1 null_blk.nr_devices=1 null_blk.queue_mode=2 null_blk.poll_queues=1"
args+=" null_blk.irqmode=0 null_blk.bs=4096"
for ((k = 1; k <= boots; k++)); do
    order="base patch"
    ((k % 2)) || order="patch base"
    for v in $order; do
        echo "BOOT $k $v"
        taskset -c "$cpu" qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G \
            -kernel "$work/build-$v/arch/x86/boot/bzImage" -initrd "$work/initramfs.cpio" -append "$args" \
            -nographic -no-reboot | tr -d '\r' | grep -a 'BENCH' | sed "s/^.*BENCH/$v BENCH/" | tee -a "$work/results"
    done
done

python3 - "$work/results" <<'EOF'
import statistics, sys
r = {}
for line in open(sys.argv[1]):
    f = line.split()
    if len(f) == 4 and f[1] == "BENCH":
        r.setdefault(f[2], {}).setdefault(f[0], []).append(float(f[3]))
    elif "ERROR" in line:
        print(line.strip())
print(f"{'benchmark':14s} {'base ns':>10s} {'range':>17s} {'patch ns':>10s} {'range':>17s} {'difference':>11s}")
for name, v in r.items():
    b, p = v.get("base", []), v.get("patch", [])
    if not b or not p:
        continue
    mb, mp = statistics.median(b), statistics.median(p)
    sep = "" if max(p) < min(b) or max(b) < min(p) else ", ranges overlap"
    print(f"{name:14s} {mb:10.2f} {min(b):8.2f}..{max(b):<8.2f} {mp:10.2f} {min(p):8.2f}..{max(p):<8.2f} "
          f"{100 * (mp - mb) / mb:+10.2f}%{sep}")
EOF
