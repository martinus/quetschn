#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# gen-tables.sh <out.c> [cc args]: seqlz's codes and decode tables as C, the seqlz_tables.c that every
# build of src/ compiles with its other files. Builds gen_seqlz_tables.c with src/seqlz_codes.c and the
# code lengths, and runs it. cc args go to that build, e.g. -DSEQLZ_PAGE_BITS=14 for 16 KiB blocks.
# The compiler is the build host's, $HOSTCC or cc, also when seqlz itself is cross-compiled: the
# tables are numbers, the same for every target.
set -euo pipefail
[[ $# -ge 1 ]] || { echo "usage: tools/gen-tables/gen-tables.sh <out.c> [cc args]" >&2; exit 2; }
out=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../src
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"${HOSTCC:-cc}" -std=gnu11 -O2 "$@" -I"$src" -o "$tmp/gen_seqlz_tables" "$here/gen_seqlz_tables.c" "$src/seqlz_codes.c" \
    "$src/seqlz_default_tables.c" "$src/seqlz_lit_sets.c"
"$tmp/gen_seqlz_tables" >"$tmp/seqlz_tables.c"
# whole or not at all, so that a make that was stopped does not take half a file for done
mv "$tmp/seqlz_tables.c" "$out"
