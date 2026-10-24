#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# series.sh <linux tree> <out dir>: the patch series for the tree's HEAD, as git format-patch writes it,
# into <out dir>, with a cover letter to fill in. One commit per patch, in a clone of the tree that shares
# its objects, so that --base names the tree's real HEAD:
#   1  x86-prefetcht0.patch, with its own changelog, unless the base has it already
#   2  everything port.py writes outside zram: lib/seqlz/ with its KUnit tests, include/linux/seqlz.h,
#      the Kconfig and Makefile lines, scripts/gen-seqlz-lengths.py, Documentation/staging/seqlz.rst and
#      MAINTAINERS; changelog from series/2-lib.txt
#   3  zram's backend: drivers/block/zram/, zram.rst and its line in the MAINTAINERS entry; changelog
#      from series/3-zram.txt
# A changelog file is the subject line, a blank line and the body; Signed-off-by comes from git config.
# Every commit is built by itself with W=1, x86-64 defconfig with zram, the backend and the KUnit tests
# as a module, and its patch checked with checkpatch --strict alone. Prints per patch the build and
# checkpatch's findings, and fails on a build error or warning, or on a changelog still marked CHANGELOG.
# check.sh and kunit.sh stay the checks of the whole; this is about each patch on its own.
# Needs about 4 GB in $TMPDIR, for the checkout and the build.
set -euo pipefail
[[ $# -eq 2 ]] || { sed -n '4,17p' "$0" >&2; exit 2; }
tree=$(cd "$1" && pwd)
out=$(realpath -m "$2")
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src=$work/src
build=$work/build
base=$(git -C "$tree" rev-parse HEAD)

git clone -q --shared --no-checkout "$tree" "$src"
git -C "$src" checkout -q --detach "$base"
git -C "$src" config maintenance.auto false
git -C "$src" config gc.auto 0
git -C "$src" config user.name "$(git -C "$repo" config user.name)"
git -C "$src" config user.email "$(git -C "$repo" config user.email)"

# a changelog file as a commit message; one still marked CHANGELOG is reported at the end
unwritten=()
commit() {
    local msg=$here/series/$1
    [[ -f $msg ]] || { echo "series/$1 is missing" >&2; exit 1; }
    ! grep -q '^CHANGELOG' "$msg" || unwritten+=("$1")
    git -C "$src" commit -q -s -F "$msg"
}

if patch -d "$src" -p1 -R -s -f --dry-run <"$here/x86-prefetcht0.patch" >/dev/null 2>&1; then
    echo "the base has x86-prefetcht0.patch"
else
    git -C "$src" am -q "$here/x86-prefetcht0.patch"
fi
python3 "$here/port.py" "$src" >/dev/null
zram=(drivers/block/zram Documentation/admin-guide/blockdev/zram.rst)
# the entry's line for the backend goes with the backend, so that each patch adds its files to MAINTAINERS
backend_line=$'F:\tdrivers/block/zram/backend_seqlz.*'
grep -qxF "$backend_line" "$src/MAINTAINERS" || { echo "MAINTAINERS has no line $backend_line" >&2; exit 1; }
cp "$src/MAINTAINERS" "$work/MAINTAINERS"
grep -vxF "$backend_line" "$work/MAINTAINERS" >"$src/MAINTAINERS"
git -C "$src" add -A
git -C "$src" reset -q -- "${zram[@]}"
commit 2-lib.txt
cp "$work/MAINTAINERS" "$src/MAINTAINERS"
git -C "$src" add -A -- "${zram[@]}" MAINTAINERS
commit 3-zram.txt
[[ -z $(git -C "$src" status --porcelain) ]] || { echo "port.py wrote files outside the patches:" >&2; git -C "$src" status --short >&2; exit 1; }

# each commit by itself: built, then its patch checked
kmake=(make -C "$src" O="$build" -j"$(nproc)")
rc=0
for c in $(git -C "$src" rev-list --reverse "$base"..HEAD); do
    git -C "$src" checkout -q "$c"
    subject=$(git -C "$src" log -1 --format=%s "$c")
    "${kmake[@]}" defconfig >/dev/null
    "$src/scripts/config" --file "$build/.config" --enable ZRAM --enable ZRAM_BACKEND_SEQLZ --module SEQLZ \
        --enable KUNIT --module SEQLZ_KUNIT_TEST
    "${kmake[@]}" olddefconfig >/dev/null
    dirs=(arch/x86/kernel/)
    [[ -d $src/lib/seqlz ]] && dirs+=(lib/seqlz/)
    dirs+=(drivers/block/zram/)
    if ! "${kmake[@]}" W=1 "${dirs[@]}" >"$work/build.log" 2>&1 || grep -qE 'warning:|error:' "$work/build.log"; then
        grep -E 'warning:|error:' "$work/build.log" | head -20
        echo "$subject: build failed or warned" >&2
        rc=1
    fi
    findings=$(git -C "$src" format-patch -1 --stdout "$c" |
        (cd "$src" && perl scripts/checkpatch.pl --strict --show-types --summary-file - || true) |
        grep -E "^(ERROR|WARNING|CHECK)" | sort | uniq -c | sort -rn || true)
    echo "$subject: built ($(IFS=' '; echo "${dirs[*]}")); checkpatch: ${findings:-clean}"
done

rm -rf "$out"
mkdir -p "$out"
git -C "$src" format-patch -q --base="$base" --cover-letter -o "$out" "$base"..HEAD
ls "$out"
if ((${#unwritten[@]})); then
    echo "changelogs still to write: ${unwritten[*]}" >&2
    rc=1
fi
exit $rc
