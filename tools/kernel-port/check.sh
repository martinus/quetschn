#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# check.sh <linux tree> [make args]: port.py into a copy of the tree's HEAD, then the checks a submission
# needs: the codec, its KUnit tests and zram built with W=1, checkpatch --strict on the patch,
# kernel-doc on the codec. make args go to every make, e.g. LLVM=1, or ARCH=arm64 LLVM=1 for arm64.
# On arm64 also the library with 16 KiB and 64 KiB pages, without the zram backend. The tests run with
# kunit.sh.
# Prints the patch's size and checkpatch's summary; fails on a build error or warning.
set -euo pipefail
[[ $# -ge 1 ]] || { echo "usage: tools/kernel-port/check.sh <linux tree> [make args]" >&2; exit 2; }
tree=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
kmake=(make -C "$work/src" O="$work/build" "$@")

# a make whose output goes to $work/build.log; stops the script on a failed build or a warning, printing
# them. Not "make | grep && exit 1": with pipefail a failed make fails the pipe, the exit never runs, and
# set -e does not stop at a command before && either
build() {
    "$@" >"$work/build.log" 2>&1 || { grep -E 'warning|error' "$work/build.log" >&2; echo "build failed" >&2; exit 1; }
    ! grep -E 'warning|error' "$work/build.log" || exit 1
}

git -C "$tree" archive HEAD | tar -x -C "$work" --one-top-level=src
# without automatic maintenance: the commit of a whole tree starts it in the background, and it still
# wrote into .git when the trap removed it, which failed the job
git -C "$work/src" init -q && git -C "$work/src" config maintenance.auto false && git -C "$work/src" config gc.auto 0
# the series' patch of arch/x86 goes into the base, so that checkpatch sees seqlz's part alone; it is
# checked by itself below. port.py leaves a tree that has it alone
patch -d "$work/src" -p1 -R -s -f --dry-run <"$here/x86-prefetcht0.patch" >/dev/null ||
    patch -d "$work/src" -p1 -s <"$here/x86-prefetcht0.patch"
git -C "$work/src" add -A && git -C "$work/src" -c user.name=port -c user.email=port@localhost commit -qm base
python3 "$here/port.py" "$work/src" >/dev/null
git -C "$work/src" add -A
git -C "$work/src" diff --cached --stat | tail -1

"${kmake[@]}" defconfig >/dev/null
"$work/src/scripts/config" --file "$work/build/.config" --enable ZRAM --enable ZRAM_BACKEND_SEQLZ --module SEQLZ \
    --enable KUNIT --module SEQLZ_KUNIT_TEST
"${kmake[@]}" olddefconfig >/dev/null
for c in ZRAM_BACKEND_SEQLZ=y SEQLZ_KUNIT_TEST=m; do
    grep -q "^CONFIG_$c" "$work/build/.config" || { echo "CONFIG_$c not set" >&2; exit 1; }
done
"${kmake[@]}" -j"$(nproc)" prepare >/dev/null
build "${kmake[@]}" -j"$(nproc)" W=1 KCFLAGS=-fstack-usage lib/seqlz/ drivers/block/zram/
ls "$work/build/lib/seqlz/seqlz_decompress.o" "$work/build/lib/seqlz/tests/seqlz_kunit.o" \
    "$work/build/drivers/block/zram/backend_seqlz.o" >/dev/null || exit 1
echo "build: no warnings"
# a division of 64 bits that the compiler leaves to a library call: the kernel has none, and only the
# link, which check.sh does not do, would say so on 32-bit arm and x86; div_u64() is the kernel's way
calls=$(nm -u "$work/build"/lib/seqlz/*.o | grep -E '__aeabi_l(div|mod)|__aeabi_uldivmod|__(u?div|u?mod)di3' || true)
[[ -z $calls ]] || { echo "lib/seqlz/ calls a 64-bit division of the libgcc: $calls" >&2; exit 1; }
# the codec's deepest stack frames, from -fstack-usage: gcc writes file:line:column:function, clang
# file:line:function. They stay below 1024 bytes, the 32-bit FRAME_WARN of older kernels;
# .claude/rules/kernel-copy.md has them with KASAN and UBSan too
frames=$(cat "$work/build"/lib/seqlz/*.su | awk -F'\t' '{ n = split($1, f, ":"); print $2, f[n] }' | sort -n -r)
echo "stack: $(head -3 <<<"$frames" | awk '{ printf "%s%s %s", (NR > 1 ? ", " : ""), $2, $1 }')"
deepest=$(head -1 <<<"$frames" | cut -d' ' -f1)
[[ $deepest =~ ^[0-9]+$ ]] || { echo "no stack usage of lib/seqlz/ found" >&2; exit 1; }
[[ $deepest -lt 1024 ]] || { echo "a stack frame of $deepest bytes, they stay below 1024" >&2; exit 1; }
# x86-64: the decoder's prefetches are there; with clang only because of x86-prefetcht0.patch
if grep -q '^CONFIG_X86_64=y' "$work/build/.config"; then
    n=$(objdump -d "$work/build/lib/seqlz/seqlz_decompress.o" | grep -c prefetcht0 || true)
    [[ $n -gt 0 ]] || { echo "lib/seqlz/seqlz_decompress.o has no prefetcht0" >&2; exit 1; }
    echo "prefetch: $n prefetcht0"
fi

# arm64: the library and its tests with 16 KiB and 64 KiB pages too, it works on blocks of 4 KiB whatever
# PAGE_SIZE is; zram's backend is for 4 KiB pages only and stays off there
if grep -q '^CONFIG_ARM64=y' "$work/build/.config"; then
    for pages in 16K 64K; do
        o="$work/build-$pages"
        pmake=(make -C "$work/src" O="$o" "$@")
        "${pmake[@]}" defconfig >/dev/null
        "$work/src/scripts/config" --file "$o/.config" --disable ARM64_4K_PAGES --enable "ARM64_${pages}_PAGES" \
            --enable ZRAM --enable ZRAM_BACKEND_SEQLZ --module SEQLZ --enable KUNIT --module SEQLZ_KUNIT_TEST
        "${pmake[@]}" olddefconfig >/dev/null
        grep -q "^CONFIG_ARM64_${pages}_PAGES=y" "$o/.config" && grep -q '^CONFIG_SEQLZ_KUNIT_TEST=m' "$o/.config" ||
            { echo "$pages pages: CONFIG_ARM64_${pages}_PAGES or CONFIG_SEQLZ_KUNIT_TEST not set" >&2; exit 1; }
        ! grep -q '^CONFIG_ZRAM_BACKEND_SEQLZ=y' "$o/.config" ||
            { echo "$pages pages: CONFIG_ZRAM_BACKEND_SEQLZ is set" >&2; exit 1; }
        "${pmake[@]}" -j"$(nproc)" prepare >/dev/null
        build "${pmake[@]}" -j"$(nproc)" W=1 lib/seqlz/
        ls "$o/lib/seqlz/seqlz_decompress.o" "$o/lib/seqlz/tests/seqlz_kunit.o" >/dev/null || exit 1
        echo "build with $pages pages: no warnings, zram's backend off"
    done
fi

# with the diffstat, as a mail of git format-patch has it: checkpatch sees in it that MAINTAINERS changes.
# Without the blank line after it, which checkpatch would take for an empty commit message
(cd "$work/src" && { git diff --cached --stat; git diff --cached; } | perl scripts/checkpatch.pl --strict --no-signoff --summary-file --show-types - || true) | grep -E "^(ERROR|WARNING|CHECK)|^total:" | sort | uniq -c | sort -rn | head -20
echo "x86-prefetcht0.patch:"
(cd "$work/src" && perl scripts/checkpatch.pl --strict --show-types "$here/x86-prefetcht0.patch" || true) | grep -E "^(ERROR|WARNING|CHECK)|^total:"
(cd "$work/src" && scripts/kernel-doc -none -Wall -Werror lib/seqlz/seqlz_codes.c lib/seqlz/seqlz_compress.c lib/seqlz/seqlz_decompress.c lib/seqlz/seqlz_internal.h lib/seqlz/seqlz.h include/linux/seqlz.h) || exit 1
echo "kernel-doc: no warnings"
# the specification as docutils reads it; Sphinx, which the kernel's htmldocs need, is not required here
if python3 -c 'import docutils' 2>/dev/null; then
    python3 - "$work/src/Documentation/staging/seqlz.rst" <<'EOF'
import io, sys
import docutils.core
err = io.StringIO()
docutils.core.publish_string(open(sys.argv[1]).read(), writer_name="null",
                             settings_overrides={"halt_level": 2, "report_level": 1, "warning_stream": err})
sys.exit(err.getvalue() or print("Documentation/staging/seqlz.rst: no docutils warnings"))
EOF
else
    echo "Documentation/staging/seqlz.rst: not checked, python3 has no docutils"
fi
