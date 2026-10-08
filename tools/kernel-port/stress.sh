#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# stress.sh <linux tree> <corpus base> <out dir>: the kernel's seqlz under the kernel's checkers, the gate
# of docs/plan.md Phase 5. port.py into a copy of the tree's HEAD, built with KASAN, lockdep
# (PROVE_LOCKING), DEBUG_ATOMIC_SLEEP, UBSan's bounds and shift checks, PREEMPT_DYNAMIC and the KUnit
# tests of seqlz, which run at boot. Then boots it in a VM, 4 CPUs and 2 GiB, once per entry of RUNS,
# with stress.c as /init: phase 1 writes every page of the corpus to new zram devices, lz4 and seqlz at
# both levels, prints mm_stat and reads them back; phase 2 swaps to zram with seqlz under memory
# pressure for MINUTES, comparing every page with what it should hold; phase 3 runs the kernel's zram
# selftests with seqlz. The VM sees the host's root read-only, for the corpus and the selftests' tools.
#
#   RUNS     preempt:level per boot, default "full:2 lazy:1"; phase 1 runs in the first boot only.
#            x86-64 has no preempt=none any more, its choices are full and lazy
#   MINUTES  of phase 2 per boot, default 10; 0 for none
#   BUILD    a build of this repository: then the zsmalloc memory the userspace model predicts for the
#            corpus is printed next to phase 1's mm_stat
#
# Writes <out dir>/boot-<preempt>-<level>.log, the console of each boot, and fails on any splat in them
# (BUG, WARNING, a lockdep or UBSan report, an oops), a RESULT FAIL line, a missing end, KUnit or
# selftest result. The logs hold no page contents. KASAN does not see a read past an object inside
# zsmalloc's pages, only past a kmalloc() buffer: the KUnit tests, with buffers of the exact size, are
# what finds those.
set -euo pipefail
[[ $# -eq 3 ]] || { sed -n '4,23p' "$0" >&2; exit 2; }
tree=$1
corpus=$(realpath "$2.pages")
out=$(realpath -m "$3")
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git -C "$tree" archive HEAD | tar -x -C "$work" --one-top-level=src
python3 "$here/port.py" "$work/src" >/dev/null
kmake=(make -C "$work/src" O="$work/build")
"${kmake[@]}" defconfig >/dev/null
"$work/src/scripts/config" --file "$work/build/.config" \
    --enable ZRAM --enable ZSMALLOC --enable ZRAM_BACKEND_LZ4 --enable ZRAM_BACKEND_SEQLZ --enable SEQLZ \
    --enable KUNIT --enable SEQLZ_KUNIT_TEST \
    --enable KASAN --enable KASAN_GENERIC --enable KASAN_INLINE \
    --enable PROVE_LOCKING --enable DEBUG_ATOMIC_SLEEP --enable DEBUG_PREEMPT \
    --enable UBSAN --enable UBSAN_BOUNDS --enable UBSAN_SHIFT \
    --enable PREEMPT_DYNAMIC --enable PREEMPT --disable PREEMPT_LAZY \
    --enable DEVTMPFS --enable BLK_DEV_INITRD --enable SWAP --enable EXT4_FS \
    --enable VIRTIO --enable VIRTIO_PCI --enable NET_9P --enable NET_9P_VIRTIO --enable 9P_FS
"${kmake[@]}" olddefconfig >/dev/null
for c in ZRAM_BACKEND_SEQLZ SEQLZ_KUNIT_TEST KASAN PROVE_LOCKING DEBUG_ATOMIC_SLEEP UBSAN_BOUNDS UBSAN_SHIFT \
    PREEMPT_DYNAMIC 9P_FS; do
    grep -q "^CONFIG_$c=y" "$work/build/.config" || { echo "CONFIG_$c is not set" >&2; exit 1; }
done
"${kmake[@]}" -j"$(nproc)" bzImage >/dev/null
grep -h "^CONFIG_CC_VERSION_TEXT" "$work/build/.config" | sed 's/^/KERNEL /'

mkdir -p "$work/root/dev" "$work/root/sys" "$work/root/proc"
cp -r "$work/src/tools/testing/selftests/zram" "$work/root/selftests"
gcc -static -O2 -Wall -o "$work/root/init" "$here/stress.c"
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null) >"$work/initramfs.cpio"

status=0
first=1
for run in ${RUNS:-full:2 lazy:1}; do
    preempt=${run%%:*} level=${run#*:}
    log="$out/boot-$preempt-$level.log"
    echo "== boot preempt=$preempt, seqlz level $level for the swap"
    qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 2G -kernel "$work/build/arch/x86/boot/bzImage" \
        -initrd "$work/initramfs.cpio" \
        -virtfs local,path=/,mount_tag=host,security_model=none,readonly=on \
        -append "console=ttyS0 panic=-1 preempt=$preempt zswap.enabled=0 zram.num_devices=4 \
quetschn.corpus=$corpus quetschn.mmstat=$first quetschn.level=$level quetschn.minutes=${MINUTES:-10} \
quetschn.selftests=1" \
        -nographic -no-reboot >"$log" 2>&1 || true
    first=0
    grep -a '^RESULT\|# seqlz: \|selftest: .*\(PASS\|FAIL\)' "$log" || true
    bad=$(grep -a -c -E 'BUG:|WARNING:|UBSAN:|Oops|general protection|possible (circular|recursive) locking|inconsistent lock state|sleeping function called from invalid context|Kernel panic|RESULT FAIL' "$log" || true)
    for need in 'RESULT done' '# seqlz: pass:6 fail:0' 'zram01 : \[PASS\]' 'zram02 : \[PASS\]'; do
        grep -a -q "$need" "$log" || { echo "missing: $need" >&2; bad=$((bad + 1)); }
    done
    if [[ $bad -ne 0 ]]; then
        echo "FAIL: $bad lines, see $log" >&2
        status=1
    else
        echo "ok, no splat"
    fi
done

if [[ -n ${BUILD:-} ]]; then
    echo "== zsmalloc memory of phase 1: mm_stat's mem_used_total against the userspace model"
    "$BUILD/quetschn-bench-interleaved" --codecs lz4,seqlz-fast,seqlz-fast-lit --corpus "${corpus%.pages}" \
        --no-timing >"$work/model.txt"
    python3 - "$work/model.txt" "$out"/boot-*.log <<'PY'
import re, sys
model = {}
codec = None
for line in open(sys.argv[1]):
    if line.startswith("codec "):
        codec = line.split()[1].rstrip(",")
    m = re.match(r"zsmalloc (cost|new device) +(\d+) bytes", line)
    if m:
        model.setdefault(codec, {})[m[1]] = int(m[2])
names = {"lz4": "lz4", "seqlz:1": "seqlz-fast", "seqlz:2": "seqlz-fast-lit"}
print(f"{'zram':10s} {'mem_used_total':>15s} {'model, new device':>18s} {'Σ zsmalloc cost':>16s}")
for log in sys.argv[2:]:
    for line in open(log, errors="replace"):
        m = re.match(r"RESULT mm_stat (\S+) .*: (.*)", line)
        if m:
            used = int(m[2].split()[2])
            mod = model[names[m[1]]]
            print(f"{m[1]:10s} {used:15d} {mod['new device']:18d} {mod['cost']:16d}  "
                  f"{100.0 * (used - mod['new device']) / mod['new device']:+.2f}%, "
                  f"{100.0 * (used - mod['cost']) / mod['cost']:+.2f}%")
PY
fi
exit $status
