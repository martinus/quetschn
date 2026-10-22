#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# fetch-kernel.sh <dir> <commit> [paths]: the kernel at <commit> into <dir>, a git checkout; only <paths>
# if any are given, a sparse checkout without the other files' contents. Tries github.com twice, then
# git.kernel.org, which serves a commit by its hash too but without the filter, so the whole tree of
# that commit, about 70 s. Each try has a time limit for the fetch and the checkout: on 22nd October
# 2026 github.com answered 504 and hung for an hour, also to the checkout of a sparse clone, which
# downloads the files' contents only then.
set -uo pipefail
[[ $# -ge 2 ]] || { echo "usage: .github/fetch-kernel.sh <dir> <commit> [paths]" >&2; exit 2; }
dir=$1
commit=$2
shift 2
paths=("$@")

# try <url> <seconds> [fetch args]: one fetch and checkout from scratch
try() {
    local url=$1 limit=$2
    shift 2
    rm -rf "$dir" && git init -q "$dir" && git -C "$dir" remote add origin "$url" || return 1
    if [[ ${#paths[@]} -gt 0 ]]; then
        git -C "$dir" sparse-checkout set --no-cone "${paths[@]}" || return 1
    fi
    timeout "$limit" git -C "$dir" fetch -q --depth 1 "$@" origin "$commit" &&
        timeout "$limit" git -C "$dir" checkout -q FETCH_HEAD
}

filter=()
[[ ${#paths[@]} -eq 0 ]] || filter=(--filter=blob:none)
for i in 1 2; do
    try https://github.com/torvalds/linux.git 180 "${filter[@]}" && exit 0
    echo "github.com, try $i: no kernel at $commit" >&2
    sleep 30
done
try https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git 900 && exit 0
echo "the kernel at $commit: neither from github.com nor from git.kernel.org" >&2
exit 1
