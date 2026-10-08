#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""port.py <linux tree>: seqlz as the kernel would have it, written into a Linux source tree.

src/ stays the one source of the codec. This writes from it:
  include/linux/seqlz.h                 the interface, tools/kernel-port/seqlz.h: compress, decompress
  lib/seqlz/seqlz.h                     from src/seqlz.h, without what only the harness uses
  lib/seqlz/seqlz_codec.c, page_lz.h    from src/seqlz.c and src/page_lz.h, and the interface's
                                        functions from tools/kernel-port/seqlz_api.c, with the tables
                                        built once
  lib/seqlz/seqlz_default_tables.c, seqlz_lit_sets.c: the tables of src/, the .inc files of 4 KiB
                                        pages in them
  lib/seqlz/tests/seqlz_kunit.c         the KUnit tests, tools/kernel-port/seqlz_kunit.c, with the
                                        tables' SHA-256 and the worked example from docs/format.md
  lib/seqlz/Makefile, .kunitconfig, lib/Kconfig, lib/Makefile: the module seqlz, CONFIG_SEQLZ, and
                                        CONFIG_SEQLZ_KUNIT_TEST
  drivers/block/zram/backend_seqlz.[ch] from tools/kernel-port/, with Kconfig, Makefile and zcomp.c
On the way: the kernel's headers instead of src/seqlz_compat.h, PAGE_SHIFT instead of
QUETSCHN_PAGE_BITS, the exports and the module's licence, the SPDX lines in the kernel's order, and no
references to this repository in the comments (its documents, tools and build); a comment that
changed is wrapped again to 80 columns. Fails if a reference is left, naming it. Writes only into the
tree; run it on a copy or a branch, tools/kernel-port/check.sh does.
"""
import pathlib
import re
import sys
import textwrap

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
SRC = REPO / "src"
WIDTH = 80

# what only the harness and the tests of src/ use, left out of the kernel's copy
HARNESS_ONLY = re.compile(r"\bseqlz_find\b|\bseqlz_encode\b|\bseqlz_sequence\b|\bSEQLZ_MAX_SEQUENCES\b|"
                          r"\bseqlz_tables_size\b")

# where the kernel's copy points for the format, the one reference to this repository it keeps
SPEC_URL = "https://github.com/martinus/quetschn/blob/main/docs/format.md"
# what may not be left in the kernel's copy
FORBIDDEN = re.compile(r"docs/|explored|quetschn|QUETSCHN|tools/|bench/|CMake|\.inc\b|src/|#\d{2,3}\b|"
                       r"seqlz-fast|zramphone|\bdump\b|\bcorpus\b")

# whole sentences or phrases of comments, as they read after the comment's lines are joined
REWRITES = [
    ("docs/format.md is the specification. docs/explored-designs.md has the measurements behind each "
     "choice, by the headings quoted below.", f"The format is specified in {SPEC_URL}."),
    (" The why of each choice below, with the numbers, is in docs/explored-designs.md.", ""),
    (" The numbers are in docs/explored-designs.md.", ""),
    (" (docs/format.md, Offsets)", ""),
    (" (#108)", ""),
    ("(src/page_lz.h)", "(page_lz.h)"),
    (", see src/seqlz_default_tables.c for the pages they are trained on", ""),
    ("; 3 / 5 and 4 / 4 are in \"seqlz, third decoder round\".", "."),
    (" (docs/explored-designs.md)", ""),
    (" The same bytes as seqlz_encode() for the sequences of seqlz_find().", ""),
    ("@t: seqlz_tables_size() bytes", "@t: the tables to build"),
    # the interface has the name seqlz_compress() now, the function of src/ is seqlz_compress_page()
    ("seqlz_compress()", "seqlz_compress_page()"),
]
# a measurement's heading in parentheses, with a number of an item or a remark after it
HEADING = re.compile(r" ?\(\"[^\"]+\"(?:,? [^)]*)?\)")
PAGE_BITS_BLOCK = re.compile(r"(/\*[^*]*?(?:\*[^/][^*]*?)*?\*/\n)?#ifndef QUETSCHN_PAGE_BITS\n#define QUETSCHN_PAGE_BITS 12[^\n]*\n#endif\n")

TABLE_HEADERS = {
    "seqlz_default_tables.c": "The code lengths compiled in, part of the format, trained on pages of\n"
                              "desktops and phones.",
    "seqlz_lit_sets.c": "The literal tables of pages with coded literals, one chosen per page, part of\n"
                        "the format: 8, by k-means over the literal histograms of the training pages.\n"
                        "The most used table first.",
}


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


# a short formula, e.g. "ml - 4 = 0", stays on one line
FORMULA = re.compile(r"\b\w+(?: [-+*/=<>]=? \w+)+")


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
                for a, b in REWRITES:
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


def top_comment(text, words):
    """the first comment after the SPDX line replaced"""
    first, rest = text.split("\n", 1)
    m = re.match(r"/\*.*?\*/\n", rest, re.S)
    assert m, "no top comment"
    body = "\n".join(" * " + l if l else " *" for l in words.split("\n"))
    return first + "\n/*\n" + body + "\n */\n" + rest[m.end():]


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


def port_header():
    """lib/seqlz/seqlz.h: what the codec's files share, not an interface"""
    t = (SRC / "seqlz.h").read_text()
    t = spdx(t, False)
    t = must(t, "#ifndef _LINUX_SEQLZ_H\n#define _LINUX_SEQLZ_H\n", "#ifndef _LIB_SEQLZ_SEQLZ_H\n#define _LIB_SEQLZ_SEQLZ_H\n")
    t = must(t, '#include "seqlz_compat.h"\n', "#include <asm/page.h>\n#include <linux/minmax.h>\n#include <linux/types.h>\n")
    t = cut(t, r"/\* a sequence as the matcher finds it, see seqlz_find\(\) \*/\nstruct seqlz_sequence \{.*?\};\n\n")
    t = cut(t, r"/\*\*\n \* seqlz_tables_size\(\) -.*?;\n\n")
    t = cut(t, r"/\*\*\n \* seqlz_encode\(\) -.*?;\n\n")
    t = cut(t, r"/\* every sequence but the last covers at least 4 bytes of the page \*/\n#define SEQLZ_MAX_SEQUENCES[^\n]*\n")
    t = cut(t, r"/\*\*\n \* seqlz_find\(\) -.*?;\n\n")
    t = rename_compress(t, ";")
    t = common(t)
    t = must(t, "#define SEQLZ_PAGE (1U << PAGE_SHIFT)\n",
             "#if PAGE_SHIFT != 12\n#error \"seqlz is for 4 KiB pages only\"\n#endif\n"
             "#define SEQLZ_PAGE (1U << PAGE_SHIFT)\n")
    return t


def port_page_lz():
    t = (SRC / "page_lz.h").read_text()
    t = spdx(t, False)
    t = must(t, '#include "seqlz_compat.h"\n', "#include <asm/page.h>\n#include <linux/string.h>\n"
             "#include <linux/types.h>\n#include <linux/unaligned.h>\n")
    return common(t)


def port_codec():
    t = (SRC / "seqlz.c").read_text()
    t = spdx(t, True)
    t = must(t, '#include "seqlz.h"\n', "#include <linux/build_bug.h>\n#include <linux/cache.h>\n"
             "#include <linux/errno.h>\n#include <linux/export.h>\n#include <linux/init.h>\n"
             "#include <linux/module.h>\n#include <linux/seqlz.h>\n\n#include \"seqlz.h\"\n")
    t = must(t, "#if defined(__KERNEL__) && defined(__aarch64__)\n", "#ifdef CONFIG_ARM64\n")
    t = must(t, "/* tests take the in-order path on any CPU with -DSEQLZ_IN_ORDER=1 */\n#ifndef SEQLZ_IN_ORDER\n"
             "#define SEQLZ_IN_ORDER 0\n#endif\n", "")
    t = must(t, "\treturn SEQLZ_IN_ORDER;\n", "\treturn 0;\n")
    # what only the harness uses: seqlz_find(), seqlz_encode() and their helpers
    t = cut(t, r"/\* ---- the matcher of page_lz\.h, for seqlz_find\(\) ---- \*/\n.*?(?=/\* ---- encoder ---- \*/)")
    t = cut(t, r"/\* seqlz_encode\(\) up to the coded literals, with the compressor's encoder \*/\n"
               r"static unsigned int encode_raw\(.*?\n\}\n\n")
    t = cut(t, r"unsigned int seqlz_encode\(.*?\n\}\n\n")
    t = cut(t, r"size_t seqlz_tables_size\(void\)\n\{\n.*?\n\}\n\n")
    # seqlz_compress() is the interface's name now, with the work memory and a level
    t = rename_compress(t, "\n{")
    t = t.rstrip("\n") + "\n\n" + (HERE / "seqlz_api.c").read_text()
    t = common(t)
    t = t.rstrip("\n") + "\n\nMODULE_LICENSE(\"Dual MIT/GPL\");\n" \
        "MODULE_DESCRIPTION(\"seqlz: LZ compression of memory pages with static Huffman codes\");\n"
    return t


def port_tables(name, inc_base, symbol):
    t = (SRC / name).read_text()
    t = spdx(t, True)
    t = top_comment(t, TABLE_HEADERS[name])
    # the KUnit tests check the tables against their SHA-256 and make pages with them
    t = must(t, '#include "seqlz.h"\n', '#include <kunit/visibility.h>\n#include <linux/export.h>\n\n'
             '#include "seqlz.h"\n')
    # 4 KiB pages only: the format for 16 KiB pages is not fixed yet (format.md, Status). The .inc file
    # without its SPDX line and comment, so that the export follows the table, as checkpatch wants it
    inc = (SRC / f"{inc_base}_4k.inc").read_text()
    inc = re.sub(r"\A//[^\n]*\n/\*.*?\*/\n", "", inc, flags=re.S)
    assert inc.startswith("const "), inc[:80]
    t = must(t, f'#if QUETSCHN_PAGE_BITS != 12\n#include "{inc_base}_16k.inc"\n#else\n'
                f'#include "{inc_base}_4k.inc"\n#endif\n', f"{inc}EXPORT_SYMBOL_IF_KUNIT({symbol});\n")
    t = common(t)
    return t


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


def patch(path, a, b, marker):
    t = path.read_text()
    if marker in t:
        sys.exit(f"{path}: already has {marker}")
    path.write_text(must(t, a, b))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tree = pathlib.Path(sys.argv[1])
    if not (tree / "drivers/block/zram/zcomp.c").exists():
        sys.exit(f"{tree} is not a Linux tree with zram")
    out = {
        "include/linux/seqlz.h": (HERE / "seqlz.h").read_text(),
        "lib/seqlz/seqlz.h": port_header(),
        "lib/seqlz/page_lz.h": port_page_lz(),
        "lib/seqlz/seqlz_codec.c": port_codec(),
        "lib/seqlz/seqlz_default_tables.c": port_tables("seqlz_default_tables.c", "seqlz_default_tables",
                                                        "seqlz_default_own"),
        "lib/seqlz/seqlz_lit_sets.c": port_tables("seqlz_lit_sets.c", "seqlz_lit_sets", "seqlz_lit_sets"),
        "lib/seqlz/tests/seqlz_kunit.c": port_kunit(),
        "lib/seqlz/Makefile": "# SPDX-License-Identifier: GPL-2.0-only OR MIT\n"
                              "ccflags-y += -O3\n\n"
                              "obj-$(CONFIG_SEQLZ) += seqlz.o\n"
                              "seqlz-y := seqlz_codec.o seqlz_default_tables.o seqlz_lit_sets.o\n\n"
                              "obj-$(CONFIG_SEQLZ_KUNIT_TEST) += tests/seqlz_kunit.o\n",
        "lib/seqlz/.kunitconfig": "CONFIG_KUNIT=y\nCONFIG_SEQLZ_KUNIT_TEST=y\n",
        "drivers/block/zram/backend_seqlz.c": (HERE / "backend_seqlz.c").read_text(),
        "drivers/block/zram/backend_seqlz.h": (HERE / "backend_seqlz.h").read_text(),
    }
    left = []
    for path, text in out.items():
        for n, line in enumerate(text.split("\n"), 1):
            if FORBIDDEN.search(line.replace(SPEC_URL, "")) or HARNESS_ONLY.search(line):
                left.append(f"{path}:{n}: {line.strip()}")
    if left:
        sys.exit("references to this repository left:\n" + "\n".join(left))
    for path, text in out.items():
        (tree / path).parent.mkdir(parents=True, exist_ok=True)
        (tree / path).write_text(text)

    patch(tree / "lib/Kconfig", "config LZ4_DECOMPRESS\n\ttristate\n\n",
          "config LZ4_DECOMPRESS\n\ttristate\n\nconfig SEQLZ\n\ttristate\n\thelp\n"
          "\t  seqlz compresses and decompresses one memory page of 4 KiB\n"
          "\t  at a time. It is an LZ format whose sequences are Huffman\n"
          "\t  coded with fixed tables, for zram. Selected by\n"
          "\t  ZRAM_BACKEND_SEQLZ.\n\n"
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
    for path in out:
        print(path)


if __name__ == "__main__":
    main()
