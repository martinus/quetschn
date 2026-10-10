#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""port.py <linux tree>: seqlz as the kernel would have it, written into a Linux source tree.

src/ stays the one source of the codec. This writes from it:
  include/linux/seqlz.h                 the interface, tools/kernel-port/seqlz.h: compress, decompress
  lib/seqlz/seqlz.h                     from src/seqlz.h, without what only the harness uses
  lib/seqlz/seqlz_internal.h            from src/, with the tables the interface's functions use
  lib/seqlz/seqlz_codes.c, seqlz_compress.c, seqlz_decompress.c: from src/, each with its part of the
                                        interface from tools/kernel-port/api_*.c; the matcher of
                                        src/page_lz.h in seqlz_compress.c, calling the encoder itself;
                                        static what only that file calls, and the tables built once,
                                        __init
  lib/seqlz/seqlz_tables.c              the tables, generated in the tree by scripts/gen-seqlz-tables.py,
                                        tools/kernel-port/gen-seqlz-tables.py, from lib/seqlz/seqlz_counts.txt,
                                        bench/seqlz_counts_4k.txt with comments of its own and lines of at
                                        most 80 columns
  lib/seqlz/tests/seqlz_kunit.c         the KUnit tests, tools/kernel-port/seqlz_kunit.c, with the
                                        tables' SHA-256 and the worked example from docs/format.md
  lib/seqlz/Makefile, .kunitconfig, lib/Kconfig, lib/Makefile: the module seqlz, CONFIG_SEQLZ, and
                                        CONFIG_SEQLZ_KUNIT_TEST
  drivers/block/zram/backend_seqlz.[ch] from tools/kernel-port/, with Kconfig, Makefile and zcomp.c; the
                                        backend in the form of the tree's zcomp, with one context per
                                        CPU or with separate ones for compression and decompression
  MAINTAINERS                           the entry of seqlz, with the project's page
  Documentation/admin-guide/blockdev/zram.rst: the backend, its page size and its levels
  arch/x86/include/asm/processor.h      x86-prefetcht0.patch, prefetch() as prefetcht0 on x86-64, which
                                        clang drops otherwise; not if the tree has it already
On the way: the kernel's headers instead of src/seqlz_compat.h, PAGE_SHIFT instead of
QUETSCHN_PAGE_BITS, the exports and the module's licence, the SPDX lines in the kernel's order, and no
references to this repository in the comments (its documents, tools and build) and no reasons that
only hold for the Mi 9T's 4.14, Fedora's config or our test machines, which say the reason in general
terms instead; a comment that changed is wrapped again to 80 columns. Fails if a reference is left,
naming it; the one URL it keeps is PROJECT_URL.
  Documentation/staging/seqlz.rst       docs/format.md as reStructuredText, see format_rst.py, in the
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
                          r"\bseqlz_tables_size\b")

# the one reference to this repository the kernel's copy keeps, the project's page in MAINTAINERS
PROJECT_URL = "https://github.com/martinus/quetschn"
# as the whole URL: with a path after it, it is not the one
KEPT_URLS = re.compile(re.escape(PROJECT_URL) + r"(?![\w/#-]|\.\w)")
SPEC = "Documentation/staging/seqlz.rst"
# nothing in lib/ depends on the CPU model: such a choice is arch code's, a static key at most
CPU_MODEL = re.compile(r"read_cpuid|\bMIDR_|boot_cpu_data|x86_model")
# what may not be left in the kernel's copy
FORBIDDEN = re.compile(r"docs/|explored|quetschn|QUETSCHN|tools/|bench/|CMake|\.inc\b|src/|#\d{2,3}\b|"
                       r"seqlz-fast|zramphone|\bdump\b|\bcorpus\b|"
                       # the reasons of an old kernel, a distribution or our test machines, which mainline
                       # does not have
                       r"4\.14|Mi 9T|Fedora|kernel VM|\bthe phone|\bthe PC\b")

# whole sentences or phrases of comments, as they read after the comment's lines are joined
REWRITES = [
    ("docs/format.md is the specification. docs/explored-designs.md has the measurements behind each "
     "choice, by the headings quoted below.", f"{SPEC} specifies the format."),
    (" The same bytes as seqlz_encode() for the sequences of seqlz_find().", ""),
    ("@t: seqlz_tables_size() bytes", "@t: the tables to build"),
    # the interface has the name seqlz_compress() now, the function of src/ is seqlz_compress_page()
    ("seqlz_compress()", "seqlz_compress_page()"),
    # the kernel's copy is for 4 KiB pages only, see port_header()
    (" For 16 KiB pages it is 16 KiB, as lz4's table.", ""),
    ("A page has at most 16 KiB of literals", "A page has at most 4 KiB of literals"),
    ("at most 640 bytes in a 4 KiB page and 2560 in a 16 KiB page, so w is at most 12.",
     "at most 640 bytes in a 4 KiB page, so w is at most 10 here; the format allows 12."),
    ("; for 16 KiB pages 15 876 bytes", ""),
    (", 60 for 16 KiB pages", ""),
    ("20 or 22 bits more", "20 bits more"),
    # port_compress() takes out raw and the check of all_symbols, which the tables built once pass
    ("a copy: raw if the caller has one, else spare, at least a page,", "a copy in spare, at least a page,"),
    # the second line of seqlz_compress_page()'s Return:, a line of its own
    ("@dst_cap bytes or @t lacks a code.", "@dst_cap bytes."),
    ("QUETSCHN_PAGE_BITS bits, 20 or 22;", "QUETSCHN_PAGE_BITS bits, 20;"),
    ("How many bits of the offset follow the token, per class: 0, 4, 8, the page's bits, 5, the page's bits "
     "- 3. One nibble per class in one constant, 0x95C840 for 4 KiB pages, 0xB5E840 for 16 KiB, so that it "
     "is a shift and a mask without a branch: computing class 3's from the page's bits made gcc branch on "
     "the class.",
     "How many bits of the offset follow the token, per class: 0, 4, 8, 12, 5 and 9. One nibble per class "
     "in one constant, so that it is a shift and a mask without a branch."),
]
# the entry in MAINTAINERS, its fields in the order the file's head gives, the files in alphabetic order
MAINTAINERS_ENTRY = f"""SEQLZ PAGE COMPRESSION
M:	Martin Leitner-Ankerl <martin.ankerl@gmail.com>
L:	linux-mm@kvack.org
L:	linux-kernel@vger.kernel.org
S:	Maintained
W:	{PROJECT_URL}
F:	Documentation/staging/seqlz.rst
F:	drivers/block/zram/backend_seqlz.*
F:	include/linux/seqlz.h
F:	lib/seqlz/
F:	scripts/gen-seqlz-tables.py
"""
# the backend in zram's documentation, after what it says about levels; the facts are the Kconfig help's
# and backend_seqlz.c's
ZRAM_RST = "Documentation/admin-guide/blockdev/zram.rst"
ZRAM_DOC = """seqlz (CONFIG_ZRAM_BACKEND_SEQLZ) is for 4 KiB pages only. It supports
`level`, but no dictionary. Level 1 stores the literals, the bytes that are
not copied from earlier in the page, as they are. Level 2, the default,
Huffman codes them where that saves space, which takes more time::

	echo "algo=seqlz level=1" > /sys/block/zram0/algorithm_params

"""
# a measurement's heading in parentheses, with a number of an item or a remark after it
HEADING = re.compile(r" ?\(\"[^\"]+\"(?:,? [^)]*)?\)")
PAGE_BITS_BLOCK = re.compile(r"(/\*[^*]*?(?:\*[^/][^*]*?)*?\*/\n)?#ifndef QUETSCHN_PAGE_BITS\n#define QUETSCHN_PAGE_BITS 12[^\n]*\n#endif\n")

# the comment of the counts in the kernel's copy, instead of the one of bench/
COUNTS_HEAD = """# The symbol counts of seqlz's tables for 4 KiB pages, which
# scripts/gen-seqlz-tables.py turns into lib/seqlz/seqlz_tables.c. Counted on
# 524 912 memory pages of desktops and a phone, with seqlz's matcher; the
# literal tables by k-means over the 440 898 pages with more than 64 literals.
# token, ll and ml: how often each symbol occurred, by number. lit: the pages of
# a literal table, then how often each byte occurred as a literal on them.
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
                t = t.replace("QUETSCHN_PAGE_BITS", "PAGE_SHIFT").replace(".inc", ".h")
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
    """seqlz_compress() of src/ as seqlz_compress_page(): the interface has the name now. Its signature,
    with end the ";" or the "{" after it, wrapped again under the longer name; the comments get the name
    from REWRITES, which wraps them again."""
    return must(text, "unsigned int seqlz_compress(const struct seqlz_tables *t,\n"
                      "\t\t\t    struct seqlz_state *st, const void *src, void *dst,\n"
                      "\t\t\t    unsigned int dst_cap, bool coded)" + end,
                "unsigned int seqlz_compress_page(const struct seqlz_tables *t,\n"
                "\t\t\t\t struct seqlz_state *st, const void *src,\n"
                "\t\t\t\t void *dst, unsigned int dst_cap, bool coded)" + end)


def common(text):
    text = PAGE_BITS_BLOCK.sub("", text)
    text = fix_comments(text)
    text = text.replace("QUETSCHN_PAGE_BITS", "PAGE_SHIFT")
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


# the kernel-doc of the functions that are static in the kernel's copy, out of lib/seqlz/seqlz.h and
# above their definitions
STATIC_DOCS = ["seqlz_all_symbols", "seqlz_tables_init", "seqlz_decode", "seqlz_compress"]


def header_docs():
    """the kernel-doc of each of STATIC_DOCS in src/seqlz.h, and the header without them and their
    prototypes"""
    t = (SRC / "seqlz.h").read_text()
    docs = {}
    for name in STATIC_DOCS:
        m = re.search(r"/\*\*\n \* " + name + r"\(\) -.*?\*/\n", t, re.S)
        assert m, name
        docs[name] = m.group(0)
        t = cut(t, re.escape(m.group(0)) + r"[^;]*;\n\n")
    return t, docs


def put_doc(text, name, doc):
    """doc above the definition of name"""
    m = re.search(r"^static [^\n]*\b" + name + r"\(", text, re.M)
    assert m, name
    return text[:m.start()] + doc + text[m.start():]


def port_header():
    """lib/seqlz/seqlz.h: what the codec's files share, not an interface"""
    t, _ = header_docs()
    t = spdx(t, False)
    t = must(t, "#ifndef _LINUX_SEQLZ_H\n#define _LINUX_SEQLZ_H\n", "#ifndef _LIB_SEQLZ_SEQLZ_H\n#define _LIB_SEQLZ_SEQLZ_H\n")
    t = must(t, '#include "seqlz_compat.h"\n', "#include <asm/page.h>\n#include <linux/minmax.h>\n#include <linux/types.h>\n")
    t = cut(t, r"/\* a sequence as the matcher finds it, see seqlz_find\(\) \*/\nstruct seqlz_sequence \{.*?\};\n\n")
    t = cut(t, r"/\*\*\n \* seqlz_tables_size\(\) -.*?;\n\n")
    t = cut(t, r"/\*\*\n \* seqlz_encode\(\) -.*?;\n\n")
    t = cut(t, r"/\* every sequence but the last covers at least 4 bytes of the page \*/\n#define SEQLZ_MAX_SEQUENCES[^\n]*\n")
    t = cut(t, r"/\*\*\n \* seqlz_find\(\) -.*?;\n\n")
    # 4 KiB pages only: the values of 16 KiB pages go, not only their comments
    t = must(t, "#define SEQLZ_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U)\n", "#define SEQLZ_HASH_BITS 12U\n")
    t, n = re.subn(r"#define SEQLZ_RAW_NIBBLES\s*\\\n[^#]*?\(QUETSCHN_PAGE_BITS - 3U\) << 20\)\n",
                   "#define SEQLZ_RAW_NIBBLES 0x95c840U\n", t)
    assert n == 1, "SEQLZ_RAW_NIBBLES"
    t = common(t)
    t = must(t, "#define SEQLZ_PAGE (1U << PAGE_SHIFT)\n",
             "#if PAGE_SHIFT != 12\n#error \"seqlz is for 4 KiB pages only\"\n#endif\n"
             "#define SEQLZ_PAGE ((unsigned int)PAGE_SIZE)\n")
    return t


def port_internal():
    """lib/seqlz/seqlz_internal.h, with the tables that the interface's functions use"""
    t = (SRC / "seqlz_internal.h").read_text()
    t = spdx(t, False)
    t = must(t, "#ifndef SEQLZ_INTERNAL_H\n#define SEQLZ_INTERNAL_H\n",
             "#ifndef _LIB_SEQLZ_SEQLZ_INTERNAL_H\n#define _LIB_SEQLZ_SEQLZ_INTERNAL_H\n")
    t = must(t, '#include "seqlz.h"\n', "#include <linux/stddef.h>\n#include <linux/types.h>\n\n#include \"seqlz.h\"\n")
    t = must(t, "\n#endif\n", "\n/* built by seqlz_init(), see seqlz_codes.c */\n"
             "extern struct seqlz_tables seqlz_fixed_tables;\n\n#endif\n")
    return common(t)


INCLUDES = {
    "codes": ["linux/bug.h", "linux/build_bug.h", "linux/errno.h", "linux/init.h", "linux/module.h", "linux/seqlz.h",
              "linux/string.h"],
    "compress": ["linux/errno.h", "linux/export.h", "linux/seqlz.h", "linux/string.h", "linux/unaligned.h"],
    "decompress": ["linux/cache.h", "linux/errno.h", "linux/export.h", "linux/prefetch.h", "linux/seqlz.h",
                   "linux/unaligned.h"],
}


def port_c(name):
    """lib/seqlz/seqlz_<name>.c from src/, with the kernel's headers and the interface's part of
    api_<name>.c"""
    t = (SRC / f"seqlz_{name}.c").read_text()
    t = spdx(t, True)
    t = must(t, '#include "seqlz_internal.h"\n',
              "".join(f"#include <{h}>\n" for h in INCLUDES[name]) + '\n#include "seqlz_internal.h"\n')
    return t


def port_codes(docs):
    t = port_c("codes")
    t = cut(t, r"size_t seqlz_tables_size\(void\)\n\{\n.*?\n\}\n\n")
    # all of it runs once, from seqlz_init()
    for name in ["length_entry", "first_codes", "build_token", "build_values", "build_lit", "build_lit_sets",
                 "has_all_symbols"]:
        m = re.search(r"^static (\w+) " + name + r"\(([^)]*)\)\n\{", t, re.M)
        assert m, name
        t = t[:m.start()] + signature(f"static {m.group(1)} __init {name}(", m.group(2), "\n{") + t[m.end():]
    for name in ["seqlz_all_symbols", "seqlz_tables_init"]:
        t = make_static(t, name, init=True)
        t = put_doc(t, name, docs[name])
    t = t.rstrip("\n") + "\n\n" + (HERE / "api_codes.c").read_text()
    return common(t)


def port_compress(docs):
    t = port_c("compress")
    # what only the harness uses: seqlz_find(), seqlz_encode() and their helpers
    t = cut(t, r"/\* ---- the matcher of page_lz\.h, for seqlz_find\(\) ---- \*/\n.*?(?=/\* ---- encoder ---- \*/)")
    t = cut(t, r"/\* seqlz_encode\(\) up to the coded literals, with the compressor's encoder \*/\n"
               r"static unsigned int encode_raw\(.*?\n\}\n\n")
    t = cut(t, r"unsigned int seqlz_encode\(.*?\n\}\n\n")
    # the tables are checked once, by seqlz_init(), not for every page
    t = must(t, "\tif (dst_cap < SEQLZ_HEADER + ENC_ROOM || !t->all_symbols)\n", "\tif (dst_cap < SEQLZ_HEADER + ENC_ROOM)\n")
    # code_literals()' copy of the raw literals is always the matcher's table: raw is the harness'
    t = must(t, "static unsigned int code_literals(const struct seqlz_tables *t, u8 *d,\n"
                "\t\t\t\t  unsigned int len, const u8 *raw, u8 *spare)\n",
             "static unsigned int code_literals(const struct seqlz_tables *t, u8 *d,\n"
             "\t\t\t\t  unsigned int len, u8 *spare)\n")
    t = must(t, "\tliterals = raw ? raw : memcpy(spare, literals, n_literals);\n",
             "\tliterals = memcpy(spare, literals, n_literals);\n")
    t = must(t, "code_literals(t, dst, len, NULL, st->literals);", "code_literals(t, dst, len, st->literals);")
    # the matcher of page_lz.h, right before the one function that calls it, calling encode_emit()
    # itself: in the kernel's copy it has no other caller
    lz = (SRC / "page_lz.h").read_text()
    lz = lz[lz.index('#include "seqlz.h"\n') + len('#include "seqlz.h"\n'):lz.rindex("#endif")].strip("\n")
    lz = cut(lz, r"/\*\n \* The matcher calls this for every sequence it finds.*?\);\n\n")
    lz = must(lz, "static __always_inline void seqlz_match_page(u16 *table, const u8 *src,\n"
                  "\t\t\t\t\t     emit_fn emit, void *ctx)\n",
              "static __always_inline void seqlz_match_page(u16 *table, const u8 *src,\n"
              "\t\t\t\t\t     struct seqlz_encoder *e)\n")
    lz = must(lz, "emit(ctx, ", "encode_emit(e, ", 2)
    t = must(t, '#include "page_lz.h"\n\n', "")
    t = must(t, "static unsigned int compress_page(", "/* ---- the matcher ---- */\n\n" + lz + "\n\n"
             "static unsigned int compress_page(")
    t = must(t, "\tseqlz_match_page(st->table, src, encode_emit, &e);\n", "\tseqlz_match_page(st->table, src, &e);\n")
    t = must(t, "static __always_inline void encode_emit(void *ctx, const u8 *in,\n"
                "\t\t\t\t\tunsigned int ll, unsigned int ml,\n\t\t\t\t\tunsigned int off)\n{\n"
                "\tstruct seqlz_encoder *e = ctx;\n",
             "static __always_inline void encode_emit(struct seqlz_encoder *e,\n"
             "\t\t\t\t\tconst u8 *in, unsigned int ll,\n\t\t\t\t\tunsigned int ml, unsigned int off)\n{\n")
    # seqlz_compress() is the interface's name now, with the work memory and a level
    t = rename_compress(t, "\n{")
    t = make_static(t, "seqlz_compress_page")
    t = put_doc(t, "seqlz_compress_page", docs["seqlz_compress"])
    t = t.rstrip("\n") + "\n\n" + (HERE / "api_compress.c").read_text()
    return common(t)


def port_decompress(docs):
    t = port_c("decompress")
    t = make_static(t, "seqlz_decode")
    t = put_doc(t, "seqlz_decode", docs["seqlz_decode"])
    t = t.rstrip("\n") + "\n\n" + (HERE / "api_decompress.c").read_text()
    return common(t)


def port_counts():
    """bench/seqlz_counts_4k.txt with the comment of the kernel's copy and at most 80 columns"""
    words = [w for line in (REPO / "bench" / "seqlz_counts_4k.txt").read_text().split("\n")
             if not line.startswith("#") for w in line.split()]
    out, line = [], ""
    for w in words:
        if w in ("token", "ll", "ml", "lit") and line:
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
    m = re.search(r"The (\d+) bytes `([0-9a-f ]+)` are a 4 KiB page of `ab`", spec)
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
    docs = header_docs()[1]
    out = {
        "include/linux/seqlz.h": (HERE / "seqlz.h").read_text(),
        "lib/seqlz/seqlz.h": port_header(),
        "lib/seqlz/seqlz_internal.h": port_internal(),
        "lib/seqlz/seqlz_codes.c": port_codes(docs),
        "lib/seqlz/seqlz_compress.c": port_compress(docs),
        "lib/seqlz/seqlz_decompress.c": port_decompress(docs),
        "lib/seqlz/seqlz_counts.txt": port_counts(),
        SPEC: ".. SPDX-License-Identifier: GPL-2.0-only OR MIT\n\n" +
              format_rst.convert((REPO / "docs" / "format.md").read_text()),
        "scripts/gen-seqlz-tables.py": (HERE / "gen-seqlz-tables.py").read_text(),
        "lib/seqlz/tests/seqlz_kunit.c": port_kunit(),
        "lib/seqlz/Makefile": "# SPDX-License-Identifier: GPL-2.0-only OR MIT\n"
                              "# as lib/lz4: with -O2 seqlz compresses slower, on x86-64 and arm64\n"
                              "ccflags-y += -O3\n\n"
                              "obj-$(CONFIG_SEQLZ) += seqlz.o\n"
                              "seqlz-y := seqlz_codes.o seqlz_compress.o seqlz_decompress.o seqlz_tables.o\n\n"
                              "obj-$(CONFIG_SEQLZ_KUNIT_TEST) += tests/seqlz_kunit.o\n",
        "lib/seqlz/.kunitconfig": "CONFIG_KUNIT=y\nCONFIG_SEQLZ_KUNIT_TEST=y\n",
        "drivers/block/zram/backend_seqlz.c": port_backend(tree),
        "drivers/block/zram/backend_seqlz.h": (HERE / "backend_seqlz.h").read_text(),
    }
    left = []
    added = {"MAINTAINERS": MAINTAINERS_ENTRY, ZRAM_RST: ZRAM_DOC}
    for path, text in [*out.items(), *added.items()]:
        for n, line in enumerate(text.split("\n"), 1):
            if FORBIDDEN.search(KEPT_URLS.sub("", line)) or HARNESS_ONLY.search(line) or \
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
    # the tables, by the generator in the tree, as anybody would make them again
    (tree / "scripts/gen-seqlz-tables.py").chmod(0o755)
    out["lib/seqlz/seqlz_tables.c"] = subprocess.run(
        [sys.executable, "scripts/gen-seqlz-tables.py", "lib/seqlz/seqlz_counts.txt"], cwd=tree, check=True,
        capture_output=True, text=True).stdout
    (tree / "lib/seqlz/seqlz_tables.c").write_text(out["lib/seqlz/seqlz_tables.c"])

    patch(tree / "lib/Kconfig", "config LZ4_DECOMPRESS\n\ttristate\n\n",
          "config LZ4_DECOMPRESS\n\ttristate\n\nconfig SEQLZ\n\ttristate\n\thelp\n"
          "\t  seqlz compresses and decompresses one memory page of 4 KiB\n"
          "\t  at a time, for zram. It is an LZ format whose sequences are\n"
          "\t  Huffman coded with fixed tables.\n\n"
          "config SEQLZ_KUNIT_TEST\n"
          "\ttristate \"KUnit tests for seqlz\" if !KUNIT_ALL_TESTS\n"
          "\tdepends on KUNIT && PAGE_SIZE_4KB\n"
          "\tdefault KUNIT_ALL_TESTS\n"
          "\tselect SEQLZ\n"
          "\tselect CRYPTO_LIB_SHA256\n"
          "\thelp\n"
          "\t  KUnit tests for seqlz: its tables against their SHA-256,\n"
          "\t  pages that are compressed and decompressed again, and pages\n"
          "\t  that are damaged or invalid by one rule of the format.\n\n"
          "\t  If unsure, say N.\n\n", "config SEQLZ\n")
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
              "\t  seqlz is an LZ codec for memory pages whose sequences are\n"
              "\t  Huffman coded with tables trained on memory pages and\n"
              "\t  compiled in. It stores pages in less memory than lz4, and\n"
              "\t  takes more time to compress and decompress them. Level 1\n"
              "\t  keeps the literals raw, level 2, the default, codes them\n"
              "\t  too.\n\n")
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
