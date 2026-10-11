#!/usr/bin/env python3
"""A copy of src/seqlz_decompress.c whose seqlz_decode() records each step, for the step-through of
chapter 14, or of src/page_lz.h whose seqlz_match_page() does, for chapter 9. STEP_OUT() or STEP_IN() goes in front of the statements below, on the same line, so
__LINE__ is the line in src/. The function's variables get initial values, so that a step before
their first assignment records 0 and not whatever was on the stack. Fails if a statement is not
found: then the list below has to follow src/.

instrument.py [decode] <src/seqlz_decompress.c> <out.c>   prints the function's first and last line
instrument.py match <src/page_lz.h> <out.h>
"""
import re
import sys

# statements a step stops in front of, as their line starts after the indentation
STOPS = [
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
# declarations that get initial values, on the same line
INITS = {
    "const u8 *lit, *lit_end, *lit_bound;": "const u8 *lit = 0, *lit_end = 0, *lit_bound = 0;",
    "struct seqlz_bit_reader br;": "struct seqlz_bit_reader br = { 0 };",
    "unsigned int n_lit, last = 1, tok, skip = 0;": "unsigned int n_lit = 0, last = 1, tok = 0, skip = 0;",
    "unsigned int nl, len, off, back, raw;": "unsigned int nl = 0, len = 0, off = 0, back = 0, raw = 0;",
}

# the matcher: STEP_A() where only the variables of the whole function exist, STEP_B() after the
# loop's own variables are all declared
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
MATCH_INITS = {
    "unsigned int pos = 1, anchor = 0, last = 1, h, cand;": "unsigned int pos = 1, anchor = 0, last = 1, h = 0, cand = 0;",
    "u64 v;": "u64 v = 0;",
    "u32 cand_bytes;": "u32 cand_bytes = 0;",
    "unsigned int h_next = seqlz_hash5(v_next), m, len;": "unsigned int h_next = seqlz_hash5(v_next), m = 0, len = 0;",
}

kind = sys.argv[1] if len(sys.argv) == 4 else "decode"
src, out = sys.argv[-2], sys.argv[-1]
lines = open(src).read().split("\n")
if kind == "match":
    first = next(i for i, l in enumerate(lines) if l.startswith("static __always_inline void seqlz_match_page("))
    last = next(i for i in range(first, len(lines)) if lines[i] == "}")
    found = {s: 0 for s, _ in MATCH_STOPS}
    for i in range(first, last + 1):
        body = lines[i].lstrip("\t")
        indent = lines[i][:len(lines[i]) - len(body)]
        if body in MATCH_INITS:
            lines[i] = indent + MATCH_INITS[body]
            found[body] = 1
            continue
        for stop, macro in MATCH_STOPS:
            if body.startswith(stop):
                lines[i] = f"{indent}{macro}(); {body}"
                found[stop] += 1
                break
    missing = [s for s, n in found.items() if n == 0] + [s for s in MATCH_INITS if s not in found]
    if missing:
        sys.exit("instrument.py: not found in seqlz_match_page(): " + "; ".join(missing))
    open(out, "w").write("\n".join(lines))
    print(first + 1, last + 1)
    sys.exit(0)
first = next(i for i, l in enumerate(lines) if l.startswith("int seqlz_decode("))
last = next(i for i in range(first, len(lines)) if lines[i] == "}")
found = {s: 0 for s in STOPS}
inits = {s: 0 for s in INITS}
depth_loop = None
for i in range(first, last + 1):
    line = lines[i]
    body = line.lstrip("\t")
    indent = line[:len(line) - len(body)]
    if body == "for (;;) {":
        depth_loop = len(indent)
    elif depth_loop is not None and body == "}" and len(indent) == depth_loop:
        depth_loop = -1  # after the loop
    in_loop = depth_loop is not None and depth_loop >= 0 and body != "for (;;) {"
    if body in INITS:
        lines[i] = indent + INITS[body]
        inits[body] += 1
        continue
    for s in STOPS:
        if body.startswith(s):
            # the escape's idx exists only inside its block
            macro = "STEP_IN" if in_loop else "STEP_OUT"
            lines[i] = f"{indent}{macro}(); {body}"
            found[s] += 1
            break
missing = [s for s, n in list(found.items()) + list(inits.items()) if n == 0]
if missing:
    sys.exit("instrument.py: not found in seqlz_decode(): " + "; ".join(missing))
open(out, "w").write("\n".join(lines))
print(first + 1, last + 1)
