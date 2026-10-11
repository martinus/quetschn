#!/usr/bin/env python3
"""A copy of src/seqlz_decompress.c whose seqlz_decode() records each step, for the step-through of
chapter 14, or of src/page_lz.h whose seqlz_match_page() does, for chapter 9. A hook goes in front
of the statements below, on the same line, so __LINE__ is the line in src/. build.py compiles the copy
with -ftrivial-auto-var-init=zero, so that a step before a variable's first assignment records 0 and
not whatever was on the stack. Fails if a statement is not found: then the list below has to follow
src/.

instrument.py decode <src/seqlz_decompress.c> <out.c>   prints the function's first and last line
instrument.py match <src/page_lz.h> <out.h>
"""
import sys

# seqlz_decode(): statements a step stops in front of, as their line starts after the indentation.
# The hook is STEP_IN() inside the loop over the sequences, where its variables exist, else STEP_OUT()
DECODE_STOPS = [
    "n_lit = get_unaligned_le16(s);",
    "q = decode_literals(t, s, src_len, n_lit, scratch);",
    "lit = scratch;",
    "lit = s + SEQLZ_HEADER;",
    "d_fast = d_end - FAST_PAGE_ROOM;",
    "tok = next_token(&br, &t->token);",
    "if (tok >> TOK_ESCAPE_AT) {",
    "idx = (unsigned int)(br.bits >>",
    "tok = token_entry(idx, 0);",
    "back = TOK_BACK(tok);",
    "raw = (unsigned int)((br.bits << TOK_N(tok)) >> back)",
    "seqlz_br_drop(&br, TOK_DROP(tok));",
    "off = back ? raw : last;",
    "nl = TOK_LL(tok);",
    "len = TOK_ML(tok);",
    "if (!(tok & (1U << TOK_VALUE_AT)) && d <= d_fast &&",
    "tok = next_token_fast(&br, &t->token, &skip);",
    "copy16(d, lit);",
    "d += nl;",
    "lit += nl;",
    "last = off;",
    "if (off - 1U >= (unsigned int)(d - (u8 *)dst))",
    "copy_match_fast(d, off, len);",
    "d += len;",
    "continue;",
    "if (nl == SEQLZ_LL_CAP) {",
    "nl += seqlz_br_value(&br, &t->ll);",
    "if (lit > lit_end || nl > (unsigned int)(lit_end - lit) ||",
    "copy_literals(d, d_end, lit, lit_bound, nl);",
    "if (d == d_end) {",
    "if (len != 4U || TOK_BACK(tok) != 0U)",
    "break;",
    "if (len == SEQLZ_ML_CAP + 4U) {",
    "len += seqlz_br_value(&br, &t->ml);",
    "if (off - 1U >= (unsigned int)(d - (u8 *)dst) ||",
    "copy_match(d, d_end, off, len);",
    "skip = 0;",
    "if (d != d_end || lit != lit_end || br.count < 0 || br.p != br.end ||",
    "return 0;",
]

# seqlz_match_page(): STEP_A() where only the variables of the whole function exist, STEP_B() after
# the loop's own variables are declared
MATCH_STOPS = [
    ("memset(table, 0, sizeof(u16) << SEQLZ_HASH_BITS);", "STEP_A"),
    ("u64 v_next = get_unaligned_le64(src + pos + 1);", "STEP_A"),
    ("if (!(rep_hit | cand_hit)) {", "STEP_B"),
    ("m = rep_hit ? pos - last : cand;", "STEP_B"),
    ("while (pos > anchor && m > 0 && src[pos - 1] == src[m - 1]) {", "STEP_B"),
    ("len = 4U + seqlz_match_len(src + pos + 4, src + m + 4,", "STEP_B"),
    ("emit(ctx, src + anchor, pos - anchor, len, last);", "STEP_B"),
    ("table[seqlz_hash5(get_unaligned_le64(src + pos - 2))] =", "STEP_B"),
    ("emit(ctx, src + anchor, SEQLZ_PAGE - anchor, 0, 0);", "STEP_A"),
]

# the function and its last line: the first "}" at the start of a line after it
FUNCTIONS = {"decode": "int seqlz_decode(", "match": "static __always_inline void seqlz_match_page("}

kind, src, out = sys.argv[1:4]
lines = open(src).read().split("\n")
first = next(i for i, l in enumerate(lines) if l.startswith(FUNCTIONS[kind]))
last = next(i for i in range(first, len(lines)) if lines[i] == "}")
stops = [(s, None) for s in DECODE_STOPS] if kind == "decode" else MATCH_STOPS
found = {s: 0 for s, _ in stops}
loop = None  # the indentation of "for (;;) {" while inside the decoder's loop, -1 after it
for i in range(first, last + 1):
    body = lines[i].lstrip("\t")
    indent = lines[i][:len(lines[i]) - len(body)]
    if body == "for (;;) {":
        loop = len(indent)
        continue
    if loop is not None and loop >= 0 and body == "}" and len(indent) == loop:
        loop = -1
    for stop, macro in stops:
        if body.startswith(stop):
            macro = macro or ("STEP_IN" if loop is not None and loop >= 0 else "STEP_OUT")
            lines[i] = f"{indent}{macro}(); {body}"
            found[stop] += 1
            break
missing = [s for s, n in found.items() if n == 0]
if missing:
    sys.exit(f"instrument.py: not found in {FUNCTIONS[kind]}...): " + "; ".join(missing))
open(out, "w").write("\n".join(lines))
print(first + 1, last + 1)
