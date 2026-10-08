#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# check.sh <linux tree> [make args]: port.py into a copy of the tree's HEAD, then the checks a submission
# needs: the codec and zram built with W=1, checkpatch --strict on the patch, kernel-doc on the
# codec. make args go to every make, e.g. LLVM=1, or ARCH=arm64 LLVM=<toolchain>/bin/ for arm64.
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
git -C "$work/src" init -q && git -C "$work/src" add -A && git -C "$work/src" -c user.name=port -c user.email=port@localhost commit -qm base
python3 "$here/port.py" "$work/src" >/dev/null
git -C "$work/src" add -A
git -C "$work/src" diff --cached --stat | tail -1

"${kmake[@]}" defconfig >/dev/null
"$work/src/scripts/config" --file "$work/build/.config" --enable ZRAM --enable ZRAM_BACKEND_SEQLZ --module SEQLZ
"${kmake[@]}" olddefconfig >/dev/null
grep -q '^CONFIG_ZRAM_BACKEND_SEQLZ=y' "$work/build/.config" || { echo "CONFIG_ZRAM_BACKEND_SEQLZ not set" >&2; exit 1; }
"${kmake[@]}" -j"$(nproc)" prepare >/dev/null
"${kmake[@]}" -j"$(nproc)" W=1 lib/seqlz/ drivers/block/zram/ 2>&1 | tee "$work/build.log" | grep -E 'warning|error' && exit 1
ls "$work/build/lib/seqlz/seqlz_codec.o" "$work/build/drivers/block/zram/backend_seqlz.o" >/dev/null && echo "build: no warnings"

(cd "$work/src" && git diff --cached | perl scripts/checkpatch.pl --strict --no-signoff --summary-file --show-types - || true) | grep -E "^(ERROR|WARNING|CHECK)|^total:" | sort | uniq -c | sort -rn | head -20
(cd "$work/src" && scripts/kernel-doc -none -Wall lib/seqlz/seqlz_codec.c lib/seqlz/seqlz.h include/linux/seqlz.h) && echo "kernel-doc: no warnings"
