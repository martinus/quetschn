#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# check.sh <linux tree> [make args]: port.py into a copy of the tree's HEAD, then the checks a submission
# needs: the codec, its KUnit tests and zram built with W=1, checkpatch --strict on the patch,
# kernel-doc on the codec. make args go to every make, e.g. LLVM=1, or ARCH=arm64 LLVM=1 for arm64.
# The tests run with kunit.sh.
# Prints the patch's size and checkpatch's summary; fails on a build error or warning.
set -euo pipefail
[[ $# -ge 1 ]] || { echo "usage: tools/kernel-port/check.sh <linux tree> [make args]" >&2; exit 2; }
tree=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
kmake=(make -C "$work/src" O="$work/build" "$@")

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
"${kmake[@]}" -j"$(nproc)" W=1 lib/seqlz/ drivers/block/zram/ 2>&1 | tee "$work/build.log" | grep -E 'warning|error' && exit 1
ls "$work/build/lib/seqlz/seqlz_decompress.o" "$work/build/lib/seqlz/tests/seqlz_kunit.o" \
    "$work/build/drivers/block/zram/backend_seqlz.o" >/dev/null && echo "build: no warnings"
# x86-64: the decoder's prefetches are there; with clang only because of x86-prefetcht0.patch
if grep -q '^CONFIG_X86_64=y' "$work/build/.config"; then
    n=$(objdump -d "$work/build/lib/seqlz/seqlz_decompress.o" | grep -c prefetcht0 || true)
    [[ $n -gt 0 ]] || { echo "lib/seqlz/seqlz_decompress.o has no prefetcht0" >&2; exit 1; }
    echo "prefetch: $n prefetcht0"
fi

# with the diffstat, as a mail of git format-patch has it: checkpatch sees in it that MAINTAINERS changes.
# Without the blank line after it, which checkpatch would take for an empty commit message
(cd "$work/src" && { git diff --cached --stat; git diff --cached; } | perl scripts/checkpatch.pl --strict --no-signoff --summary-file --show-types - || true) | grep -E "^(ERROR|WARNING|CHECK)|^total:" | sort | uniq -c | sort -rn | head -20
echo "x86-prefetcht0.patch:"
(cd "$work/src" && perl scripts/checkpatch.pl --strict --show-types "$here/x86-prefetcht0.patch" || true) | grep -E "^(ERROR|WARNING|CHECK)|^total:"
(cd "$work/src" && scripts/kernel-doc -none -Wall lib/seqlz/seqlz_codes.c lib/seqlz/seqlz_compress.c lib/seqlz/seqlz_decompress.c lib/seqlz/seqlz_internal.h lib/seqlz/seqlz.h include/linux/seqlz.h) && echo "kernel-doc: no warnings"
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
