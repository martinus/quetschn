#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# samecode.sh <linux tree> <old> [<new>]: the machine code of the kernel copy, function by function, old
# against new, for a refactor that should not change it. <old> and <new> are each a directory with this
# repository (a worktree) or a revision of it; <new> is this worktree by default, with what is not
# committed. Each goes through its own tools/kernel-port/port.py into a copy of the tree's HEAD, and
# lib/seqlz/ and zram's backend are built with W=1 as the kernel builds them: gcc and clang for x86-64,
# clang for arm64 (ARCHS="x86 x86-clang arm64"). Prints per build "same code" or the functions that
# differ, and fails then; SHOW=1 prints their diff, MAP="old=new ..." names renamed functions.
# The copy of the tree and the builds stay in $SAMECODE_CACHE (default ~/.cache/quetschn-samecode,
# about 3 GB) for the next call, until the tree's HEAD changes. It compiles: not while a benchmark times.
set -euo pipefail
[[ $# -eq 2 || $# -eq 3 ]] || { echo "usage: [ARCHS=...] [SHOW=1] [MAP='old=new ...'] tools/samecode/samecode.sh <linux tree> <old> [<new>]" >&2; exit 2; }
tree=$1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
cache=${SAMECODE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/quetschn-samecode}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# a directory as it is, a revision as git archive writes it
checkout() {
    if [[ -d $1 ]]; then
        cd "$1" && pwd
    else
        mkdir -p "$work/rev-$2"
        git -C "$repo" archive "$1" | tar -x -C "$work/rev-$2"
        echo "$work/rev-$2"
    fi
}
declare -A dir=([old]=$(checkout "$2" old) [new]=$(checkout "${3:-$repo}" new))

head=$(git -C "$tree" rev-parse HEAD)
if [[ $(cat "$cache/head" 2>/dev/null) != "$head" ]]; then
    rm -rf "$cache"
    mkdir -p "$cache/src"
    git -C "$tree" archive HEAD | tar -x -C "$cache/src"
    python3 "${dir[new]}/tools/kernel-port/port.py" "$cache/src" >/dev/null
    echo "$head" >"$cache/head"
fi

# the files port.py reads or writes outside lib/seqlz/, from the tree's HEAD
files=(drivers/block/zram lib/Kconfig lib/Makefile MAINTAINERS Documentation/admin-guide/blockdev/zram.rst
    Documentation/staging/index.rst arch/x86/include/asm/processor.h)
rc=0
for a in ${ARCHS:-x86 x86-clang arm64}; do
    case $a in
    x86) mk=() od=objdump ;;
    x86-clang) mk=(LLVM=1) od=llvm-objdump ;;
    arm64) mk=(ARCH=arm64 LLVM=1) od=llvm-objdump ;;
    *) echo "unknown arch $a" >&2; exit 2 ;;
    esac
    kmake=(make -C "$cache/src" O="$cache/build-$a" "${mk[@]}")
    for v in old new; do
        # the copy as $v's port.py writes it, over the kept tree
        rm -rf "$work/copy" && mkdir "$work/copy"
        git -C "$tree" archive HEAD "${files[@]}" | tar -x -C "$work/copy"
        python3 "${dir[$v]}/tools/kernel-port/port.py" "$work/copy" >/dev/null
        rm -rf "$cache/src/lib/seqlz"
        cp -r "$work/copy/." "$cache/src/"
        if [[ ! -f $cache/build-$a/.config ]]; then
            "${kmake[@]}" defconfig >/dev/null
            "$cache/src/scripts/config" --file "$cache/build-$a/.config" --enable ZRAM --enable ZRAM_BACKEND_SEQLZ --module SEQLZ
        fi
        "${kmake[@]}" olddefconfig >/dev/null
        "${kmake[@]}" -j"$(nproc)" prepare >/dev/null 2>&1
        rm -rf "$cache/build-$a/lib/seqlz" "$cache/build-$a/drivers/block/zram/backend_seqlz.o"
        if ! "${kmake[@]}" -j"$(nproc)" W=1 lib/seqlz/ drivers/block/zram/ >"$work/build.log" 2>&1 ||
            grep -q warning "$work/build.log"; then
            grep -E 'warning|error' "$work/build.log" | head -20
            echo "build of $v failed or warned: $a" >&2
            exit 2
        fi
        # seqlz.o is the module, all the others linked; the backend is the one file of zram that changes
        for o in "$cache/build-$a"/lib/seqlz/*.o "$cache/build-$a/drivers/block/zram/backend_seqlz.o"; do
            case ${o##*/} in seqlz.o | *.mod.o) continue ;; esac
            $od -dr --no-show-raw-insn "$o"
            $od -s -j .rodata "$o" 2>/dev/null || true
        done >"$work/$v.dis"
    done
    python3 "$here/samecode.py" ${SHOW:+--show} "$work/old.dis" "$work/new.dis" "$a" || rc=1
done
exit $rc
