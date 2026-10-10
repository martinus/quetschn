# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""docs/format.md as reStructuredText for the kernel's Documentation/, for port.py.

Only what format.md uses: headings of 3 levels, paragraphs, lists with "- " and "1. ", pipe tables,
fenced code blocks, inline code, bold, italics, and links to its own headings or to http(s). Anything
else fails, so that a new construct in format.md is noticed instead of turned into wrong reST.

Two markers, HTML comments that GitHub does not show, keep the repository's and the kernel's text
apart:

    <!-- not in the kernel's copy -->
    ... text only for this repository ...
    <!-- end -->

    <!-- in the kernel's copy:
    ... text only for the kernel, in Markdown ...
    -->
"""
import re
import textwrap

WIDTH = 80

SKIP_BEGIN = "<!-- not in the kernel's copy -->"
SKIP_END = "<!-- end -->"
ONLY_BEGIN = "<!-- in the kernel's copy:"
ONLY_END = "-->"


def select(md):
    """the lines of the kernel's copy: the repository's parts out, the kernel's in"""
    out, mode = [], None
    for n, line in enumerate(md.split("\n"), 1):
        if line == SKIP_BEGIN or line == ONLY_BEGIN:
            if mode:
                raise ValueError(f"format.md:{n}: a marker inside a marker")
            mode = "skip" if line == SKIP_BEGIN else "only"
        elif mode == "skip" and line == SKIP_END or mode == "only" and line == ONLY_END:
            mode = None
        elif "<!--" in line or "-->" in line:
            raise ValueError(f"format.md:{n}: an HTML comment that is no marker: {line}")
        elif mode != "skip":
            out.append(line)
    if mode:
        raise ValueError("format.md: a marker without its end")
    return out


def slug(title):
    """GitHub's anchor of a heading"""
    return re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")


def inline(text, titles):
    """the inline Markdown of one line or cell as reST"""
    parts = re.split(r"(`[^`]+`)", text)
    out = []
    for p in parts:
        if p.startswith("`") and p.endswith("`") and len(p) > 1:
            out.append("``" + p[1:-1] + "``")
            continue

        def link(m):
            label, target = m.group(1), m.group(2)
            if target.startswith("#"):
                if target[1:] not in titles:
                    raise ValueError(f"a link to no heading: {m.group(0)}")
                title = titles[target[1:]]
                return f"`{label}`_" if label == title else f"`{label} <{title}_>`_"
            if re.match(r"https?://", target):
                return f"`{label} <{target}>`__"
            raise ValueError(f"a link into the repository: {m.group(0)}")

        p = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, p)
        # a word that ends in _ is a reference in reST, | a substitution
        if "](" in p or "|" in p or re.search(r"\w_(\W|$)", p) or "\\" in p:
            raise ValueError(f"text reST would read otherwise: {p}")
        out.append(p)
    return "".join(out)


def table(rows, titles):
    cells = [[inline(c.strip(), titles) for c in r.strip().strip("|").split("|")] for r in rows]
    head, rule, body = cells[0], cells[1], cells[2:]
    if not all(re.fullmatch(r"\s*:?-+:?\s*", c) for c in rows[1].strip().strip("|").split("|")):
        raise ValueError(f"no table rule: {rows[1]}")
    width = [max(len(r[k]) for r in [head] + body) for k in range(len(head))]
    if any(len(r) != len(head) for r in body):
        raise ValueError(f"a table row with another number of cells: {rows[0]}")

    def line(ch):
        return "+" + "+".join(ch * (w + 2) for w in width) + "+"

    def row(r):
        return "|" + "|".join(f" {c.ljust(w)} " for c, w in zip(r, width)) + "|"

    del rule
    return [line("-"), row(head), line("=")] + [x for r in body for x in (row(r), line("-"))]


def wrap(text, first, rest):
    """text wrapped to WIDTH columns, the first line after first, the others after rest; never inside an
    inline literal or a link"""
    keep = re.sub(r"``.*?``|`[^`]*`__?", lambda m: m.group(0).replace(" ", "\0"), text)
    lines = textwrap.wrap(keep, WIDTH, initial_indent=first, subsequent_indent=rest, break_long_words=False,
                          break_on_hyphens=False)
    return [line.replace("\0", " ") for line in lines]


def paragraphs(lines):
    """the lines of plain text, paragraphs and list items each joined and wrapped again"""
    out, cur = [], None
    for line in lines + [""]:
        m = re.match(r"(- |\d+\. )(.*)", line)
        if m:
            if cur:
                out += wrap(*cur)
            cur = [m.group(2), m.group(1), " " * len(m.group(1))]
        elif line.strip() and cur and line.startswith(cur[2]) and cur[2].strip() == "" and cur[2]:
            cur[0] += " " + line.strip()
        elif line.strip() and cur and not cur[2]:
            cur[0] += " " + line.strip()
        elif line.strip():
            if cur:
                out += wrap(*cur)
            cur = [line.strip(), "", ""]
        else:
            if cur:
                out += wrap(*cur)
                cur = None
            out.append("")
    return out[:-1]


def convert(md):
    """format.md as reST"""
    lines = select(md)
    titles = {}
    for line in lines:
        m = re.match(r"(#{1,3}) (.*)", line)
        if m:
            titles[slug(m.group(2))] = m.group(2)
    out, k = [], 0
    while k < len(lines):
        line = lines[k]
        m = re.match(r"(#{1,3}) (.*)", line)
        if m:
            title = m.group(2)
            if "`" in title or "*" in title:
                raise ValueError(f"a heading with markup: {title}")
            bar = {1: "=", 2: "=", 3: "-"}[len(m.group(1))] * len(title)
            out += [bar, title, bar] if len(m.group(1)) == 1 else [title, bar]
            k += 1
        elif line.startswith("```"):
            end = lines.index("```", k + 1)
            out += ["::", ""] + ["    " + c if c else "" for c in lines[k + 1:end]]
            k = end + 1
        elif line.startswith("|"):
            end = k
            while end < len(lines) and lines[end].startswith("|"):
                end += 1
            out += table(lines[k:end], titles)
            k = end
        elif line.startswith(">"):
            raise ValueError(f"a block quote: {line}")
        else:
            end = k
            while end < len(lines) and not re.match(r"#{1,3} |```|\||>", lines[end]):
                end += 1
            out += paragraphs([inline(x, titles) if x.strip() else "" for x in lines[k:end]])
            k = end
    text = "\n".join(out).strip("\n")
    return re.sub(r"\n{3,}", "\n\n", text) + "\n"
