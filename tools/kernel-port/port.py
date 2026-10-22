#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""port.py <linux tree>: seqlz as the kernel would have it, written into a Linux source tree.

src/ stays the one source of the codec. This writes from it:
  include/linux/seqlz.h                 the interface, tools/kernel-port/seqlz.h: compress, decompress
  lib/seqlz/seqlz_format.h, seqlz_tables.h, seqlz_internal.h: from src/, without what only the harness
                                        uses
  lib/seqlz/seqlz_compress.c, seqlz_compress_literals.c, seqlz_decompress.c,
  seqlz_decompress_literals.c:          from src/, the two main files each with its part of the
                                        interface from tools/kernel-port/api_*.c; the matcher of
                                        src/seqlz_match.h in seqlz_compress.c, calling the encoder itself
  lib/seqlz/gen_seqlz_tables.c          src/seqlz_codes.c and tools/kernel-port/gen_tables_main.c, a
                                        program for the build host that writes the codes and decode
                                        tables, seqlz_tables.c, when the kernel is built
  lib/seqlz/seqlz_lengths.h             the code lengths it builds them from, written in the tree by
                                        scripts/gen-seqlz-lengths.py, tools/kernel-port/gen-seqlz-lengths.py,
                                        from lib/seqlz/seqlz_counts.txt, bench/seqlz_counts_4k.txt with
                                        comments of its own and lines of at most 80 columns
  lib/seqlz/tests/seqlz_kunit.c         the KUnit tests, tools/kernel-port/seqlz_kunit.c, with the
                                        tables' SHA-256 and the worked example from docs/format.md
  lib/seqlz/Makefile, .kunitconfig, .gitignore, lib/Kconfig, lib/Kconfig.debug, lib/Makefile: the module seqlz,
                                        CONFIG_SEQLZ, and CONFIG_SEQLZ_KUNIT_TEST
  drivers/block/zram/backend_seqlz.[ch] from tools/kernel-port/, with Kconfig, Makefile and zcomp.c; the
                                        backend in the form of the tree's zcomp, with one context per
                                        CPU or with separate ones for compression and decompression
  MAINTAINERS                           the entry of seqlz, with the project's page
  Documentation/admin-guide/blockdev/zram.rst: the backend, its page size and its levels
  arch/x86/include/asm/processor.h      x86-prefetcht0.patch, prefetch() as prefetcht0 on x86-64, which
                                        clang drops otherwise; not if the tree has it already
On the way: the kernel's headers instead of src/seqlz_compat.h, blocks of 4 KiB whatever PAGE_SIZE is,
SEQLZ_BLOCK_BITS 12 and SEQLZ_BLOCK_SIZE of the interface, the exports and the module's licence, the SPDX lines in the kernel's order, and no
references to this repository in the comments (its documents, tools and build) and no reasons that
only hold for the Mi 9T's 4.14, Fedora's config or our test machines, which say the reason in general
terms instead; a comment that changed is wrapped again to 80 columns. Fails if a reference is left,
naming it; the one URL it keeps is PROJECT_URL.
  Documentation/staging/seqlz.rst       docs/format.md as reStructuredText, see format_rst.py, and the
                                        interface's kernel-doc, in the
                                        index of Documentation/staging/
Writes only into the tree; run it on a copy or a branch, tools/kernel-port/check.sh does.
"""
import pathlib
import re
import subprocess
import sys
import textwrap

import format_rst

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
SRC = REPO / "src"
WIDTH = 80

# what only the harness and the tests of src/ use, left out of the kernel's copy
HARNESS_ONLY = re.compile(r"\bseqlz_find\b|\bseqlz_encode\b|\bseqlz_sequence\b|\bSEQLZ_MAX_SEQUENCES\b|"
                          r"\bseqlz_tables_size\b|\ball_symbols\b")

# the one reference to this repository the kernel's copy keeps, the project's page in MAINTAINERS
PROJECT_URL = "https://github.com/martinus/quetschn"
# as the whole URL: with a path after it, it is not the one
KEPT_URLS = re.compile(re.escape(PROJECT_URL) + r"(?![\w/#-]|\.\w)")
SPEC = "Documentation/staging/seqlz.rst"
# the functions' kernel-doc, from the header, at the end of the specification
SPEC_API = """
Interface
=========

.. kernel-doc:: include/linux/seqlz.h
"""
# nothing in lib/ depends on the CPU model: such a choice is arch code's, a static key at most
CPU_MODEL = re.compile(r"read_cpuid|\bMIDR_|boot_cpu_data|x86_model")
# what may not be left in the kernel's copy
FORBIDDEN = re.compile(r"docs/|explored|quetschn|QUETSCHN|tools/|bench/|CMake|\.inc\b|src/|#\d{2,3}\b|"
                       r"seqlz-fast|seqlz-[0-9]\b|zramphone|\bdump\b|\bcorpus\b|"
                       # the reasons of an old kernel, a distribution or our test machines, which mainline
                       # does not have
                       r"4\.14|Mi 9T|Fedora|kernel VM|\bthe phone|\bthe PC\b")

# whole sentences or phrases of comments, as they read after the comment's lines are joined
REWRITES = [
    ("docs/format.md is the specification.", f"{SPEC} specifies the format."),
    # the interface has the name seqlz_compress() now, the function of src/ is seqlz_compress_block()
    ("seqlz_compress()", "seqlz_compress_block()"),
    # the library works on blocks of 4 KiB whatever PAGE_SIZE is, see port_header()
    ("seqlz compresses one block at a time, a memory page for zram.",
     "seqlz compresses one block of SEQLZ_BLOCK_SIZE bytes at a time, for zram, whatever the kernel's PAGE_SIZE "
     "is."),
    # the kernel's copy is for 4 KiB pages only, see port_header()
    (" For blocks of 16 KiB it is 16 KiB, as lz4's table.", ""),
    ("A block has at most 16 KiB of literals", "A block has at most 4 KiB of literals"),
    ("at most 640 bytes in a 4 KiB block and 2560 in a 16 KiB block, so w is at most 12.",
     "at most 640 bytes in a 4 KiB block, so w is at most 10 here; the format allows 12."),
    ("; for blocks of 16 KiB 15 872 bytes", ""),
    (", 60 for 16 KiB blocks", ""),
    ("20 or 22 bits more", "20 bits more"),
    ("SEQLZ_BLOCK_BITS bits, 20 or 22;", "SEQLZ_BLOCK_BITS bits, 20;"),
    # the numbers of blocks of 4 KiB, where src/'s are for both sizes
    ("as it has at most 60 bits (see below)", "as it has at most 58 bits (see below)"),
    ("two fast sequences take at most 50, 11 for each token and up to 14 for each offset",
     "two fast sequences take at most 46, 11 for each token and up to 12 for each offset"),
]
# the entry in MAINTAINERS, its fields in the order the file's head gives, the files in alphabetic order
MAINTAINERS_ENTRY = f"""SEQLZ COMPRESSION LIBRARY
M:	Martin Leitner-Ankerl <martin.ankerl@gmail.com>
L:	linux-mm@kvack.org
L:	linux-kernel@vger.kernel.org
S:	Maintained
W:	{PROJECT_URL}
F:	Documentation/staging/seqlz.rst
F:	drivers/block/zram/backend_seqlz.*
F:	include/linux/seqlz.h
F:	lib/seqlz/
F:	scripts/gen-seqlz-lengths.py
"""
# the backend in zram's documentation, after what it says about levels; the facts are the Kconfig help's
# and backend_seqlz.c's
ZRAM_RST = "Documentation/admin-guide/blockdev/zram.rst"
ZRAM_DOC = """seqlz (CONFIG_ZRAM_BACKEND_SEQLZ) is for 4 KiB pages only. It supports
`level`, but no dictionary. Level 1 stores the literals, the bytes that are
not copied from earlier in the page, as they are. Level 2, the default,
Huffman codes them where that saves space, which takes more time. Level 3
also searches longer for matches, can store the literals as their difference
to an earlier byte and gives a page a code of its own for its literals, which
saves more space and takes more time again::

	echo "algo=seqlz level=1" > /sys/block/zram0/algorithm_params

"""
# a measurement's heading in parentheses, with a number of an item or a remark after it
HEADING = re.compile(r" ?\(\"[^\"]+\"(?:,? [^)]*)?\)")
# the block size of src/, a setting of the build, and what the kernel's copy has instead: blocks of 4 KiB,
# whatever PAGE_SIZE is, SEQLZ_BLOCK_SIZE of the interface
BLOCK_BITS_SRC = re.compile(r"/\*\n \* A block is what seqlz compresses at a time.*?\*/\n#ifndef QUETSCHN_PAGE_BITS\n"
                            r"#define QUETSCHN_PAGE_BITS 12\n#endif\n#define SEQLZ_BLOCK_BITS QUETSCHN_PAGE_BITS\n"
                            r"#define SEQLZ_BLOCK_SIZE \(1U << SEQLZ_BLOCK_BITS\)\n", re.S)
BLOCK_BITS_KERNEL = """/*
 * A block is what seqlz compresses at a time: SEQLZ_BLOCK_SIZE bytes of the
 * interface, a memory page for zram with 4 KiB pages, whatever the kernel's
 * PAGE_SIZE is. SEQLZ_BLOCK_BITS is its log2.
 */
#define SEQLZ_BLOCK_BITS 12U
"""

# the comment of the counts in the kernel's copy, instead of the one of bench/
COUNTS_HEAD = """# The symbol counts of seqlz's tables for blocks of 4 KiB, which
# scripts/gen-seqlz-lengths.py turns into lib/seqlz/seqlz_lengths.h. Counted on
# 1 091 983 memory pages of a desktop and a phone, with seqlz's matcher; the
# literal tables by k-means over the 963 723 pages with more than 64 literals.
# token, ll and ml: how often each symbol occurred, by number. lit: the pages of
# a literal table, then how often each byte occurred as a literal on them. own:
# how often each symbol occurred in the pages' own literal tables at level 3.
"""


def visual(s):
    col = 0
    for ch in s:
        col = (col // 8 + 1) * 8 if ch == "\t" else col + 1
    return col


def comment_lines(comment):
    lines = comment[2:-2].split("\n")
    out = []
    for i, line in enumerate(lines):
        t = line.strip(" \t") if i else line.strip(" ")
        if i and t.startswith("*"):
            t = t[1:]
        out.append(t[1:] if t.startswith(" ") else t)
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return [t.rstrip() for t in out]


def structured(t):
    # kernel-doc's @argument lines and Return: stay lines of their own
    return t[:1] == " " or "  " in t.strip() or t.startswith(("- ", "@", "Return:", "Context:"))


def paragraphs(lines):
    out, cur = [], None
    for t in lines:
        if not t:
            out.append(["blank", ""])
            cur = None
        elif t.startswith("- ") and "  " not in t[2:]:
            cur = ["item", t[2:]]
            out.append(cur)
        elif cur and cur[0] == "item" and t.startswith("  ") and "  " not in t.strip():
            cur[1] += " " + t.strip()
        elif structured(t):
            out.append(["keep", t])
            cur = None
        elif cur and cur[0] == "para":
            cur[1] += " " + t
        else:
            cur = ["para", t]
            out.append(cur)
    return out


# a short formula, e.g. "ml - 4 = 0", stays on one line, and so does a size, e.g. "4 KiB"
FORMULA = re.compile(r"\b\w+(?: [-+*/=<>]=? \w+)+|\b\d+ KiB\b")


def wrap(text, width):
    text = FORMULA.sub(lambda m: m.group(0).replace(" ", "\0"), text)
    return [w.replace("\0", " ") for w in textwrap.wrap(text, width, break_long_words=False, break_on_hyphens=False)]


def render(indent, paras, kerneldoc):
    room = WIDTH - visual(indent) - 3
    flat = [p for p in paras if p[0] != "blank"]
    if not kerneldoc and len(flat) == 1 and flat[0][0] == "para" and visual(indent) + len(flat[0][1]) + 6 <= WIDTH:
        return indent + "/* " + flat[0][1] + " */"
    out = [indent + ("/**" if kerneldoc else "/*")]
    for kind, text in paras:
        if kind == "blank":
            out.append(indent + " *")
        elif kind == "keep":
            out.append(indent + " * " + text)
        elif kind == "item":
            for k, w in enumerate(wrap(text, room - 2)):
                out.append(indent + " * " + ("- " if k == 0 else "  ") + w)
        else:
            out += [indent + " * " + w for w in wrap(text, room)]
    out.append(indent + " */")
    return "\n".join(out)


# how often each of REWRITES matched, over all files: one that matches nothing is stale, see main()
rewrites_used = [0] * len(REWRITES)


def fix_comments(text):
    """the rewrites inside comments; a comment that changed is wrapped again"""

    def one(m):
        indent, comment = m.group(1), m.group(2)
        kerneldoc = comment.startswith("/**")
        lines = comment_lines(comment[1:] if kerneldoc else comment)
        paras = paragraphs(lines)
        changed = False
        for p in paras:
            if p[0] in ("para", "item", "keep"):
                t = p[1]
                for k, (a, b) in enumerate(REWRITES):
                    rewrites_used[k] += t.count(a)
                    t = t.replace(a, b)
                t = HEADING.sub("", t)
                t = t.replace(".inc", ".h")
                if t != p[1]:
                    p[1], changed = t, True
        if not changed:
            return m.group(0)
        # paragraphs that are empty now go, and with them the blank lines around them
        paras = [p for p in paras if p[0] == "blank" or p[1].strip()]
        tidy = []
        for p in paras:
            if p[0] == "blank" and (not tidy or tidy[-1][0] == "blank"):
                continue
            tidy.append(p)
        while tidy and tidy[-1][0] == "blank":
            tidy.pop()
        return render(indent, tidy, kerneldoc)

    return re.sub(r"(?m)^([ \t]*)(/\*.*?\*/)", one, text, flags=re.S)


def spdx(text, c_style):
    first, rest = text.split("\n", 1)
    assert "SPDX-License-Identifier: MIT OR GPL-2.0-only" in first, first
    return ("// SPDX-License-Identifier: GPL-2.0-only OR MIT" if c_style else
            "/* SPDX-License-Identifier: GPL-2.0-only OR MIT */") + "\n" + rest


def cut(text, pattern):
    """the one match of the regular expression pattern, across lines, removed"""
    found = re.findall(pattern, text, re.S)
    assert len(found) == 1, (pattern, len(found))
    return re.sub(pattern, "", text, flags=re.S)


def must(text, a, b, count=1):
    assert text.count(a) == count, (a, text.count(a))
    return text.replace(a, b)


def rename_compress(text, end):
    """seqlz_compress() of src/ as seqlz_compress_block(): the interface has the name now. Its signature,
    with end the ";" or the "{" after it, wrapped again under the longer name; the comments get the name
    from REWRITES, which wraps them again."""
    return must(text, "unsigned int seqlz_compress(struct seqlz_state *st, const void *src, void *dst,\n"
                      "\t\t\t    unsigned int dst_cap, unsigned int level)" + end,
                "unsigned int seqlz_compress_block(struct seqlz_state *st, const void *src,\n"
                "\t\t\t\t void *dst, unsigned int dst_cap,\n"
                "\t\t\t\t unsigned int level)" + end)


def common(text):
    text = fix_comments(text)
    return text


def signature(head, params, end):
    """a function's head, e.g. "static int __init name(", and its parameters, wrapped as the kernel
    wraps them: at most WIDTH columns, the next lines under the first parameter"""
    params = [x.strip() for x in " ".join(params.split()).split(",")]
    col = visual(head)
    indent = "\t" * (col // 8) + " " * (col % 8)
    lines, cur = [], head
    for k, prm in enumerate(params):
        piece = prm + ("," if k + 1 < len(params) else ")" + end)
        if cur not in (head, indent) and visual(cur + " " + piece) > WIDTH:
            lines.append(cur.rstrip())
            cur = indent
        cur += piece if cur in (head, indent) else " " + piece
    lines.append(cur)
    return "\n".join(lines)


def make_static(text, name, init=False):
    """the definition of name static, and __init if init, its parameters wrapped again"""
    m = re.search(r"^((?:const )?(?:unsigned )?\w+ \**)" + name + r"\(([^)]*)\)\n\{", text, re.M)
    assert m, name
    head = "static " + m.group(1) + ("__init " if init else "") + name + "("
    return text[:m.start()] + signature(head, m.group(2), "\n{") + text[m.end():]


def put_doc(text, name, doc):
    """doc above the definition of name"""
    m = re.search(r"^static [^\n]*\b" + name + r"\(", text, re.M)
    assert m, name
    return text[:m.start()] + doc + text[m.start():]


def guard(t, name):
    """the include guard of src/'s name.h as the kernel's, _LIB_SEQLZ_NAME_H"""
    ours = name.upper() + "_H"
    return must(t, f"#ifndef {ours}\n#define {ours}\n", f"#ifndef _LIB_SEQLZ_{ours}\n#define _LIB_SEQLZ_{ours}\n")


def port_format():
    """lib/seqlz/seqlz_format.h: the format's numbers; with seqlz_tables.h it builds for the host too, for
    gen_seqlz_tables, so only headers that tools/include has"""
    t = spdx((SRC / "seqlz_format.h").read_text(), False)
    t = guard(t, "seqlz_format")
    t = must(t, '#include "seqlz_compat.h"\n', "#include <linux/build_bug.h>\n#include <linux/types.h>\n")
    t, k = BLOCK_BITS_SRC.subn(BLOCK_BITS_KERNEL, t)
    assert k == 1, "the block size of src/seqlz_format.h"
    return common(t)


def port_tables_h():
    """lib/seqlz/seqlz_tables.h: the tables' layout, and the tables themselves, generated"""
    t = spdx((SRC / "seqlz_tables.h").read_text(), False)
    t = guard(t, "seqlz_tables")
    # whether every symbol has a code: gen_seqlz_tables checks that when it builds the tables
    t = must(t, "\tbool all_symbols; /* see seqlz_all_symbols() */\n", "")
    t = must(t, "/* the tables of seqlz_tables_init(), read only after that, by all CPUs */\n"
                "extern struct seqlz_tables seqlz_tables;\n",
             "/* the tables, generated when the kernel is built, see gen_seqlz_tables.c */\n"
             "extern const struct seqlz_tables seqlz_tables;\n")
    return common(t)


def port_internal():
    """lib/seqlz/seqlz_internal.h: what the codec's files share"""
    t = spdx((SRC / "seqlz_internal.h").read_text(), False)
    t = guard(t, "seqlz_internal")
    t = must(t, '#include "seqlz_tables.h"\n', "#include <linux/cache.h>\n#include <linux/prefetch.h>\n"
             "#include <linux/seqlz.h>\n#include <linux/stddef.h>\n#include <linux/types.h>\n\n#include \"seqlz_tables.h\"\n")
    # blocks of 4 KiB only, whatever PAGE_SIZE is: the values of 16 KiB pages go, not only their comments
    t = must(t, "#define SEQLZ_HASH_BITS (SEQLZ_BLOCK_BITS == 12 ? 12U : 13U)\n", "#define SEQLZ_HASH_BITS 12U\n")
    t = must(t, "#define SEQLZ_LEVELS 3U\n", "#define SEQLZ_LEVELS SEQLZ_LEVEL_MAX\n")
    # the cast is for C++, which src/ is compiled as too
    t = must(t, "\tconst u8 *q = (const u8 *)p; /* a cast for the C++ of the harness */\n", "\tconst u8 *q = p;\n")
    # raw literals of their own come only from the harness's seqlz_encode()
    t = must(t, "unsigned int seqlz_code_literals(u8 *d, unsigned int len, const u8 *raw,\n\t\t\t\t u8 *spare);\n",
             "unsigned int seqlz_code_literals(u8 *d, unsigned int len, u8 *spare);\n")
    return common(t)


# what each file uses itself, not what it gets through another header
INCLUDES = {
    "compress": ["linux/errno.h", "linux/export.h", "linux/minmax.h", "linux/string.h", "linux/unaligned.h"],
    "compress_literals": ["linux/math64.h", "linux/minmax.h", "linux/string.h", "linux/unaligned.h"],
    "decompress": ["linux/errno.h", "linux/export.h", "linux/module.h", "linux/unaligned.h"],
    "decompress_literals": ["linux/compiler.h", "linux/minmax.h", "linux/unaligned.h"],
}


def port_c(name):
    """lib/seqlz/seqlz_<name>.c from src/, with the kernel's headers"""
    t = spdx((SRC / f"seqlz_{name}.c").read_text(), True)
    ours = '#include "seqlz.h"\n' if '#include "seqlz.h"\n' in t else '#include "seqlz_internal.h"\n'
    return must(t, ours, "".join(f"#include <{h}>\n" for h in INCLUDES[name]) + '\n#include "seqlz_internal.h"\n')


def port_compress_literals():
    """lib/seqlz/seqlz_compress_literals.c: seqlz_code_literals() without raw, which only the harness has"""
    t = port_c("compress_literals")
    t = must(t, "unsigned int seqlz_code_literals(u8 *d, unsigned int len, const u8 *raw,\n\t\t\t\t u8 *spare)\n"
                "{\n\treturn code_literals(d, len, raw, spare, NULL, 0);\n}\n",
             "unsigned int seqlz_code_literals(u8 *d, unsigned int len, u8 *spare)\n"
             "{\n\treturn code_literals(d, len, NULL, spare, NULL, 0);\n}\n")
    return common(t)


def port_compress():
    t = port_c("compress")
    t = must(t, "\treturn seqlz_code_literals(dst, len, NULL, st->literals);\n",
             "\treturn seqlz_code_literals(dst, len, st->literals);\n")
    # what only the harness uses: seqlz_find(), seqlz_encode() and their helpers
    t = cut(t, r"/\* ---- the matcher of seqlz_match\.h, for seqlz_find\(\) ---- \*/\n.*?(?=/\* ---- encoder ---- \*/)")
    t = cut(t, r"/\* seqlz_encode\(\) up to the coded literals, with the compressor's encoder \*/\n"
               r"static unsigned int encode_raw\(.*?\n\}\n\n")
    t = cut(t, r"unsigned int seqlz_encode\(.*?\n\}\n\n")
    # the tables are checked when they are built, not for every block
    t = must(t, "\tif (dst_cap < SEQLZ_HEADER + ENC_ROOM || !seqlz_tables.all_symbols)\n",
             "\tif (dst_cap < SEQLZ_HEADER + ENC_ROOM)\n", 2)
    # seqlz_token() of src/seqlz.h, for the encoder, its one user here
    api = (SRC / "seqlz.h").read_text()
    m = re.search(r"/\* the token of a sequence with a match, see the top of seqlz_format\.h \*/\n"
                  r"static inline unsigned int seqlz_token\(.*?\n\}\n", api, re.S)
    assert m, "seqlz_token() in src/seqlz.h"
    token = m.group(0)
    t = must(t, "/* ---- encoder ---- */\n", "/* ---- encoder ---- */\n\n" + token)
    # the matcher of seqlz_match.h, right before the one function that calls it, calling encode_seq()
    # itself: in the kernel's copy it has no other caller
    lz = (SRC / "seqlz_match.h").read_text()
    lz = lz[lz.index('#include "seqlz_internal.h"\n') + len('#include "seqlz_internal.h"\n'):lz.rindex("#endif")].strip("\n")
    lz = cut(lz, r"/\*\n \* The matcher calls this for every sequence it finds.*?\);\n\n")
    lz = must(lz, "static __always_inline void\n"
                  "seqlz_match_block(u16 *table, const u8 *src, bool lazy, emit_fn emit, void *ctx)\n",
              "static __always_inline void seqlz_match_block(u16 *table, const u8 *src,\n"
              "\t\t\t\t\t      bool lazy,\n"
              "\t\t\t\t\t      struct seqlz_encoder *e)\n")
    # encode_seq() with the records of SEQLZ_LEVEL_OWN in the lazy copy, lazy a constant there
    lz, k = re.subn(r"\bemit\(ctx, (.*?)\);", r"encode_seq(e, \1, lazy);", lz)
    assert k == 2, k
    t = must(t, '#include "seqlz_match.h"\n', "")
    t = must(t, "static unsigned int compress_block(", "/* ---- the matcher ---- */\n\n" + lz + "\n\n"
             "static unsigned int compress_block(")
    t = must(t, "\t\tseqlz_match_block(st->table, src, true, encode_emit_rec, &e);\n",
             "\t\tseqlz_match_block(st->table, src, true, &e);\n")
    t = must(t, "\tseqlz_match_block(st->table, src, false, encode_emit, &e);\n",
             "\tseqlz_match_block(st->table, src, false, &e);\n")
    # the matcher's callbacks: the kernel's copy calls encode_seq() itself
    t = cut(t, r"/\* encode_seq\(\) as the matcher calls it, without and with records \*/\n.*?"
               r"encode_seq\(ctx, in, ll, ml, off, true\);\n\}\n\n")
    # seqlz_compress() is the interface's name now, with the work memory and a level
    t = rename_compress(t, "\n{")
    t = make_static(t, "seqlz_compress_block")
    # the interface's kernel-doc is in include/linux/seqlz.h, and seqlz_compress() checked the level
    t = put_doc(t, "seqlz_compress_block", "/* one block, for the interface below, which checked the level */\n")
    t = must(t, "\tif (level < 1 || level > SEQLZ_LEVELS)\n\t\treturn 0;\n", "")
    t = t.rstrip("\n") + "\n\n" + (HERE / "api_compress.c").read_text()
    return common(t)


def port_decompress():
    t = port_c("decompress")
    t = make_static(t, "seqlz_decode")
    t = put_doc(t, "seqlz_decode", "/* seqlz_decompress(), see include/linux/seqlz.h */\n")
    t = t.rstrip("\n") + "\n\n" + (HERE / "api_decompress.c").read_text()
    return common(t)


def port_gen():
    """lib/seqlz/gen_seqlz_tables.c: src/seqlz_codes.c as a program for the build host, which writes the
    codes and decode tables as C, with tools/kernel-port/gen_tables_main.c"""
    t = spdx((SRC / "seqlz_codes.c").read_text(), True)
    t = must(t, "/*\n * The codes and decode tables, built from the code lengths:\n * seqlz_tables_init().\n */\n"
                '#include "seqlz.h"\n',
             "/*\n * gen_seqlz_tables: seqlz's codes and decode tables, built from the code\n"
             " * lengths of seqlz_lengths.h and printed as lib/seqlz/seqlz_tables.c. A program\n"
             " * for the build host, run when the kernel is built.\n */\n"
             "#include <errno.h>\n#include <stdio.h>\n#include <string.h>\n\n"
             '#include "seqlz_tables.h"\n#include "seqlz_lengths.h"\n')
    t = cut(t, r"bool seqlz_all_symbols\(void\)\n\{.*?\n\}\n\n")
    t = cut(t, r"struct seqlz_tables seqlz_tables;\n\n")
    t = cut(t, r"\n\nint seqlz_tables_init\(const struct seqlz_lengths \*lengths\)\n\{.*?\n\}\n")
    # every symbol needs a code, for the encoder
    t = must(t, "\tt->all_symbols = has_all_symbols(lengths);\n", "\tif (!has_all_symbols(lengths))\n\t\treturn -EINVAL;\n")
    t = t.rstrip("\n") + "\n\n" + (HERE / "gen_tables_main.c").read_text()
    return common(t)


# lib/seqlz/Makefile: the module, and its tables, which gen_seqlz_tables, a program for the build host,
# writes from the code lengths, as lib/crc's gen_crc32table and lib/raid's mktables do
MAKEFILE = """# SPDX-License-Identifier: GPL-2.0-only OR MIT
# as lib/lz4: with -O2 seqlz compresses slower, on x86-64 and arm64
ccflags-y += -O3

obj-$(CONFIG_SEQLZ) += seqlz.o
seqlz-y := seqlz_compress.o seqlz_compress_literals.o seqlz_decompress.o \\
	   seqlz_decompress_literals.o seqlz_tables.o

# the codes and decode tables, built from the code lengths of seqlz_lengths.h
hostprogs := gen_seqlz_tables
HOSTCFLAGS_gen_seqlz_tables.o := -I$(srctree)/tools/include

quiet_cmd_gen_seqlz_tables = GEN     $@
      cmd_gen_seqlz_tables = $(obj)/gen_seqlz_tables > $@

targets += seqlz_tables.c
$(obj)/seqlz_tables.c: $(obj)/gen_seqlz_tables FORCE
	$(call if_changed,gen_seqlz_tables)

obj-$(CONFIG_SEQLZ_KUNIT_TEST) += tests/seqlz_kunit.o
"""


def port_lengths_script():
    """scripts/gen-seqlz-lengths.py without what only the repository's userspace build uses"""
    t = (HERE / "gen-seqlz-lengths.py").read_text()
    t = cut(t, r"def main_inc\(what, tables\):\n.*?\n\n\n(?=def main\(\):)")
    t = must(t, '    what = None\n    if args and args[0] in ("--lengths", "--lit-sets"):\n'
             "        what = args.pop(0)\n", "")
    t = must(t, "    if what:\n        return main_inc(what, tables)\n", "")
    t = must(t, "def lengths_initializer(tables, storage=\"\"):", "def lengths_initializer(tables, storage):")
    t = must(t, "def lit_sets_initializer(tables, storage=\"\"):", "def lit_sets_initializer(tables, storage):")
    t = must(t, "def own_initializer(tables, storage=\"\"):", "def own_initializer(tables, storage):")
    return t


def port_counts():
    """bench/seqlz_counts_4k.txt with the comment of the kernel's copy and at most 80 columns"""
    words = [w for line in (REPO / "bench" / "seqlz_counts_4k.txt").read_text().split("\n")
             if not line.startswith("#") for w in line.split()]
    out, line = [], ""
    for w in words:
        if w in ("token", "ll", "ml", "lit", "own") and line:
            out.append(line)
            line = ""
        if line and len(line) + 1 + len(w) > WIDTH:
            out.append(line)
            line = ""
        line = f"{line} {w}" if line else w
    out.append(line)
    return COUNTS_HEAD + "\n".join(out) + "\n"


def port_kunit():
    """the KUnit tests, with what they check against from docs/format.md: the tables' SHA-256 and the
    bytes of the worked example"""
    t = (HERE / "seqlz_kunit.c").read_text()
    spec = (REPO / "docs" / "format.md").read_text()
    for key, sha in re.findall(r"^\| `(\w+_4k)` \| `([0-9a-f]{64})` \|", spec, re.MULTILINE):
        t = must(t, f'"@{key}@"', f'"{sha}"')
    m = re.search(r"The (\d+) bytes `([0-9a-f ]+)` are a 4 KiB block of `ab`", spec)
    assert m, "no example in docs/format.md"
    example = ["0x" + b for b in m.group(2).split()]
    assert len(example) == int(m.group(1)), m.group(0)
    lines = [", ".join(example[k:k + 8]) for k in range(0, len(example), 8)]
    t = must(t, "\t@EXAMPLE@\n", "".join(f"\t{l},\n" for l in lines))
    assert "@" not in t, "a value of docs/format.md not found"
    return t


def zcomp_split(tree):
    """whether the tree's zram has separate compression and decompression contexts, as Sergey
    Senozhatsky's series of October 2026 makes them"""
    return "struct zcomp_cstrm" in (tree / "drivers/block/zram/zcomp.h").read_text()


def resolve(text, split):
    """the template's ZCOMP_RW_SPLIT parts kept for a tree with the split, the others for one without,
    and every #ifdef, #ifndef, #else and #endif of it gone; nothing nested"""
    out, keep = [], None
    for line in text.split("\n"):
        if line in ("#ifdef ZCOMP_RW_SPLIT", "#ifndef ZCOMP_RW_SPLIT"):
            assert keep is None, "nested ZCOMP_RW_SPLIT"
            keep = split if line.startswith("#ifdef") else not split
        elif line == "#else" and keep is not None:
            keep = not keep
        elif line == "#endif" and keep is not None:
            keep = None
        elif keep is None or keep:
            out.append(line)
    assert keep is None, "ZCOMP_RW_SPLIT without #endif"
    # a part that went can leave two blank lines in a row
    return re.sub(r"\n{3,}", "\n\n", "\n".join(out))


def port_backend(tree):
    """backend_seqlz.c for the tree's zcomp, without the template's comment"""
    t = (HERE / "backend_seqlz.c").read_text()
    t = cut(t, r"/\*\n \* port\.py keeps the ZCOMP_RW_SPLIT parts.*?\*/\n")
    t = resolve(t, zcomp_split(tree))
    assert "ZCOMP_RW_SPLIT" not in t
    return t


def patch(path, a, b, marker):
    t = path.read_text()
    if marker in t:
        sys.exit(f"{path}: already has {marker}")
    path.write_text(must(t, a, b))


def apply_patch(tree, path):
    """a patch of the series that is not seqlz's own, unless the tree has it: then it applies in reverse"""
    patch = ["patch", "-p1", "-s", "-f", "-d", str(tree), "-i", str(path)]
    if subprocess.run(patch + ["-R", "--dry-run"], capture_output=True).returncode == 0:
        return
    subprocess.run(patch + ["-N"], check=True)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tree = pathlib.Path(sys.argv[1])
    if not (tree / "drivers/block/zram/zcomp.c").exists():
        sys.exit(f"{tree} is not a Linux tree with zram")
    out = {
        "include/linux/seqlz.h": (HERE / "seqlz.h").read_text(),
        "lib/seqlz/seqlz_format.h": port_format(),
        "lib/seqlz/seqlz_tables.h": port_tables_h(),
        "lib/seqlz/seqlz_internal.h": port_internal(),
        "lib/seqlz/seqlz_compress.c": port_compress(),
        "lib/seqlz/seqlz_compress_literals.c": port_compress_literals(),
        "lib/seqlz/seqlz_decompress.c": port_decompress(),
        "lib/seqlz/seqlz_decompress_literals.c": common(port_c("decompress_literals")),
        "lib/seqlz/gen_seqlz_tables.c": port_gen(),
        "lib/seqlz/seqlz_counts.txt": port_counts(),
        SPEC: ".. SPDX-License-Identifier: GPL-2.0-only OR MIT\n\n" +
              format_rst.convert((REPO / "docs" / "format.md").read_text()) + SPEC_API,
        "scripts/gen-seqlz-lengths.py": port_lengths_script(),
        "lib/seqlz/tests/seqlz_kunit.c": port_kunit(),
        "lib/seqlz/Makefile": MAKEFILE,
        "lib/seqlz/.kunitconfig": "CONFIG_KUNIT=y\nCONFIG_SEQLZ_KUNIT_TEST=y\n",
        # what the build generates, as lib/crc/.gitignore
        "lib/seqlz/.gitignore": "# SPDX-License-Identifier: GPL-2.0-only\n/gen_seqlz_tables\n/seqlz_tables.c\n",
        "drivers/block/zram/backend_seqlz.c": port_backend(tree),
        "drivers/block/zram/backend_seqlz.h": (HERE / "backend_seqlz.h").read_text(),
    }
    left = []
    added = {"MAINTAINERS": MAINTAINERS_ENTRY, ZRAM_RST: ZRAM_DOC}
    for path, text in [*out.items(), *added.items()]:
        for n, line in enumerate(text.split("\n"), 1):
            # the kernel's own tools/include, for the host program that writes the tables
            if FORBIDDEN.search(KEPT_URLS.sub("", line).replace("$(srctree)/tools/include", "")) or \
                    HARNESS_ONLY.search(line) or \
                    path.startswith("lib/") and CPU_MODEL.search(line):
                left.append(f"{path}:{n}: {line.strip()}")
    if left:
        sys.exit("references to this repository, reasons of our machines or a CPU model in lib/ left:\n" + "\n".join(left))
    stale = [a for (a, _), n in zip(REWRITES, rewrites_used) if n == 0]
    if stale:
        sys.exit("REWRITES that match no comment any more:\n" + "\n".join(stale))
    for path, text in out.items():
        (tree / path).parent.mkdir(parents=True, exist_ok=True)
        (tree / path).write_text(text)
    # the code lengths, by the script in the tree, as anybody would make them again; the codes and decode
    # tables are generated from them when the kernel is built, see MAKEFILE
    (tree / "scripts/gen-seqlz-lengths.py").chmod(0o755)
    out["lib/seqlz/seqlz_lengths.h"] = subprocess.run(
        [sys.executable, "scripts/gen-seqlz-lengths.py", "lib/seqlz/seqlz_counts.txt"], cwd=tree, check=True,
        capture_output=True, text=True).stdout
    (tree / "lib/seqlz/seqlz_lengths.h").write_text(out["lib/seqlz/seqlz_lengths.h"])

    patch(tree / "lib/Kconfig", "config LZ4_DECOMPRESS\n\ttristate\n\n",
          "config LZ4_DECOMPRESS\n\ttristate\n\nconfig SEQLZ\n\ttristate\n\thelp\n"
          "\t  seqlz compresses and decompresses one block of 4 KiB at a\n"
          "\t  time, for zram, whatever the page size. It is an LZ format\n"
          "\t  whose sequences and literals are Huffman coded with fixed\n"
          "\t  tables, see Documentation/staging/seqlz.rst.\n\n", "config SEQLZ\n")
    # with the other KUnit tests of lib/, in the runtime testing menu
    patch(tree / "lib/Kconfig.debug", "config PRANDOM_KUNIT_TEST\n",
          "config SEQLZ_KUNIT_TEST\n"
          "\ttristate \"KUnit tests for seqlz\" if !KUNIT_ALL_TESTS\n"
          "\tdepends on KUNIT\n"
          "\tdefault KUNIT_ALL_TESTS\n"
          "\tselect SEQLZ\n"
          "\tselect CRYPTO_LIB_SHA256\n"
          "\thelp\n"
          "\t  KUnit tests for seqlz: its tables against their SHA-256,\n"
          "\t  blocks that are compressed and decompressed again, and\n"
          "\t  blocks that are damaged or invalid by one rule of the format.\n\n"
          "\t  If unsure, say N.\n\n"
          "config PRANDOM_KUNIT_TEST\n", "config SEQLZ_KUNIT_TEST\n")
    patch(tree / "Documentation/staging/index.rst", "   rpmsg\n   speculation\n",
          "   rpmsg\n   seqlz\n   speculation\n", "   seqlz\n")
    patch(tree / "lib/Makefile", "obj-$(CONFIG_LZ4_DECOMPRESS) += lz4/\n",
          "obj-$(CONFIG_LZ4_DECOMPRESS) += lz4/\nobj-$(CONFIG_SEQLZ) += seqlz/\n", "CONFIG_SEQLZ")
    z = tree / "drivers/block/zram"
    kc = (z / "Kconfig").read_text()
    if "ZRAM_BACKEND_SEQLZ" in kc:
        sys.exit("drivers/block/zram/Kconfig already has ZRAM_BACKEND_SEQLZ")
    kc = must(kc, "\tselect 842_DECOMPRESS\n\n", "\tselect 842_DECOMPRESS\n\n"
              "config ZRAM_BACKEND_SEQLZ\n"
              "\tbool \"seqlz compression support\"\n"
              "\tdepends on ZRAM\n"
              "\tdepends on PAGE_SIZE_4KB\n"
              "\tselect SEQLZ\n"
              "\thelp\n"
              "\t  seqlz is an LZ codec whose sequences and literals are\n"
              "\t  Huffman coded with tables trained on memory pages and\n"
              "\t  compiled in. It stores pages in less memory than lz4, and\n"
              "\t  takes more time to compress and decompress them. Its levels\n"
              "\t  are in Documentation/admin-guide/blockdev/zram.rst.\n\n")
    kc = must(kc, "\t\t!ZRAM_BACKEND_842\n", "\t\t!ZRAM_BACKEND_842 && !ZRAM_BACKEND_SEQLZ\n")
    # without help, as the other ZRAM_DEF_COMP_* entries: checkpatch's CONFIG_DESCRIPTION stays
    kc = must(kc, "config ZRAM_DEF_COMP_842\n\tbool \"842\"\n\tdepends on ZRAM_BACKEND_842\n\n",
              "config ZRAM_DEF_COMP_842\n\tbool \"842\"\n\tdepends on ZRAM_BACKEND_842\n\n"
              "config ZRAM_DEF_COMP_SEQLZ\n\tbool \"seqlz\"\n\tdepends on ZRAM_BACKEND_SEQLZ\n\n")
    kc = must(kc, "\tdefault \"842\" if ZRAM_DEF_COMP_842\n",
              "\tdefault \"842\" if ZRAM_DEF_COMP_842\n\tdefault \"seqlz\" if ZRAM_DEF_COMP_SEQLZ\n")
    (z / "Kconfig").write_text(kc)
    patch(z / "Makefile", "zram-$(CONFIG_ZRAM_BACKEND_842)\t\t+= backend_842.o\n",
          "zram-$(CONFIG_ZRAM_BACKEND_842)\t\t+= backend_842.o\n"
          "zram-$(CONFIG_ZRAM_BACKEND_SEQLZ)\t+= backend_seqlz.o\n", "backend_seqlz")
    zc = (z / "zcomp.c").read_text()
    zc = must(zc, '#include "backend_842.h"\n', '#include "backend_842.h"\n#include "backend_seqlz.h"\n')
    zc = must(zc, "#if IS_ENABLED(CONFIG_ZRAM_BACKEND_842)\n\t&backend_842,\n#endif\n",
              "#if IS_ENABLED(CONFIG_ZRAM_BACKEND_842)\n\t&backend_842,\n#endif\n"
              "#if IS_ENABLED(CONFIG_ZRAM_BACKEND_SEQLZ)\n\t&backend_seqlz,\n#endif\n")
    (z / "zcomp.c").write_text(zc)
    # between its neighbours, so that an entry added there fails here instead of breaking the order
    patch(tree / "MAINTAINERS", "F:\tdrivers/iio/chemical/sps30_serial.c\n\nSERIAL DEVICE BUS\n",
          "F:\tdrivers/iio/chemical/sps30_serial.c\n\n" + MAINTAINERS_ENTRY + "\nSERIAL DEVICE BUS\n",
          "F:\tlib/seqlz/")
    patch(tree / ZRAM_RST, "the value the lower the compression ratio).\n\n",
          "the value the lower the compression ratio).\n\n" + ZRAM_DOC, "CONFIG_ZRAM_BACKEND_SEQLZ")
    apply_patch(tree, HERE / "x86-prefetcht0.patch")
    for path in out:
        print(path)


if __name__ == "__main__":
    main()
