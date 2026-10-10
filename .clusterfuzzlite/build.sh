#!/bin/bash -eu
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# The fuzz targets of fuzz/ for ClusterFuzzLite, with its compiler, sanitizer flags and libFuzzer, into
# $OUT, with make_seeds.c's seeds and the input lengths fuzz/afl.sh gives them. 4 KiB pages only; the
# fuzz job in ci.yml has 16 KiB pages and the in-order decoder too.
codec="src/seqlz_codes.c src/seqlz_compress.c src/seqlz_decompress.c src/seqlz_default_tables.c src/seqlz_lit_sets.c"
for t in decode roundtrip diff encode; do
    extra=""
    if [ "$t" = diff ]; then
        # the decoder written from docs/format.md
        extra="-Itools/seqlz-ref tools/seqlz-ref/seqlz_ref.c"
    fi
    # shellcheck disable=SC2086
    $CC $CFLAGS -Isrc $extra "fuzz/seqlz_${t}_fuzz.c" $codec -o "$OUT/seqlz_${t}_fuzz" $LIB_FUZZING_ENGINE
done

# the seeds come from a build without the sanitizer, it only writes files
seeds=$(mktemp -d)
clang -O2 -Isrc fuzz/make_seeds.c $codec -o "$seeds/make_seeds"
mkdir "$seeds/decode" "$seeds/roundtrip"
"$seeds/make_seeds" "$seeds/decode" "$seeds/roundtrip"
for t in decode roundtrip; do
    (cd "$seeds/$t" && zip -q "$OUT/seqlz_${t}_fuzz_seed_corpus.zip" ./*)
done
# diff takes compressed pages, as decode does
cp "$OUT/seqlz_decode_fuzz_seed_corpus.zip" "$OUT/seqlz_diff_fuzz_seed_corpus.zip"

# compressed pages up to twice a page, which covers lengths the decoder has to reject, and a page and
# its first byte for the roundtrip
printf '[libfuzzer]\nmax_len = 8192\n' > "$OUT/seqlz_decode_fuzz.options"
printf '[libfuzzer]\nmax_len = 8192\n' > "$OUT/seqlz_diff_fuzz.options"
printf '[libfuzzer]\nmax_len = 4097\n' > "$OUT/seqlz_roundtrip_fuzz.options"
