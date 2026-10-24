#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# series.sh <linux tree> <out dir>: the patch series for the tree's HEAD, as git format-patch writes it,
# into <out dir>, with a cover letter to fill in. One commit per patch, in a clone of the tree that shares
# its objects, so that --base names the tree's real HEAD:
#   1  x86-prefetcht0.patch, with its own changelog, unless the base has it already
#   2  the library: lib/seqlz/ without its tests, include/linux/seqlz.h, the Kconfig and Makefile lines,
#      scripts/gen-seqlz-lengths.py, Documentation/staging/seqlz.rst and MAINTAINERS; series/2-lib.txt
#   3  its KUnit tests: lib/seqlz/tests/, .kunitconfig, their lines in lib/seqlz/Makefile and
#      lib/Kconfig.debug; series/3-kunit.txt
#   4  zram's backend: drivers/block/zram/, zram.rst and its line in the MAINTAINERS entry;
#      series/4-zram.txt
# A changelog file is the subject line, a blank line and the body; Signed-off-by comes from git config.
# Every commit is built by itself with W=1, x86-64 defconfig with zram, the backend and the KUnit tests
# as a module, the library with CONFIG_SEQLZ=m also where nothing selects it yet, and each C file the
# commit adds has to give an object; its patch is checked with checkpatch --strict alone. Prints per patch the build and
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
kunit=(lib/seqlz/tests lib/seqlz/.kunitconfig lib/Kconfig.debug)
# A file that two patches change is staged first without the later patch's lines: the MAINTAINERS
# entry's line for the backend goes with the backend, lib/seqlz/Makefile's line for the tests with them.
backend_line=$'F:\tdrivers/block/zram/backend_seqlz.*'
# the Makefile's line as it is, not expanded
# shellcheck disable=SC2016
kunit_line='obj-$(CONFIG_SEQLZ_KUNIT_TEST) += tests/seqlz_kunit.o'
without() {
    grep -qxF "$2" "$src/$1" || { echo "$1 has no line $2" >&2; exit 1; }
    cp "$src/$1" "$work/${1//\//_}"
    # the line, and a blank line right before it
    python3 - "$src/$1" "$2" <<'PY'
import sys
path, line = sys.argv[1], sys.argv[2]
lines = open(path).read().split("\n")
i = lines.index(line)
del lines[i - 1 if i and not lines[i - 1] else i:i + 1]
open(path, "w").write("\n".join(lines))
PY
}
restore() { cp "$work/${1//\//_}" "$src/$1"; }
without MAINTAINERS "$backend_line"
without lib/seqlz/Makefile "$kunit_line"
git -C "$src" add -A
git -C "$src" reset -q -- "${zram[@]}" "${kunit[@]}"
commit 2-lib.txt
restore lib/seqlz/Makefile
git -C "$src" add -A -- "${kunit[@]}" lib/seqlz/Makefile
commit 3-kunit.txt
restore MAINTAINERS
git -C "$src" add -A -- "${zram[@]}" MAINTAINERS
commit 4-zram.txt
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
    rm -rf "$build/lib/seqlz" "$build"/drivers/block/zram/*.o
    if ! "${kmake[@]}" W=1 CONFIG_SEQLZ=m "${dirs[@]}" >"$work/build.log" 2>&1 ||
        grep -qE 'warning:|error:' "$work/build.log"; then
        grep -E 'warning:|error:' "$work/build.log" | head -20
        echo "$subject: build failed or warned" >&2
        rc=1
    fi
    # what the commit adds was compiled, not skipped for want of a config; gen_seqlz_tables is the host's
    for f in $(git -C "$src" diff --name-only --diff-filter=A "$c^" "$c" -- '*.c'); do
        [[ $f == */gen_seqlz_tables.c ]] && continue
        [[ -f $build/${f%.c}.o ]] || { echo "$subject: $f was not compiled" >&2; rc=1; }
    done
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
