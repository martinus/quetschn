#!/usr/bin/env python3
"""Builds the book: chapters/*.md -> seqlz-line-by-line.html.

Directives, each on a line of its own, outside fenced code:

  @listing src/page_lz.h 13-22
      the lines 13 to 22 of that file, as they are in the repository, with their
      real line numbers and syntax highlighting

  ::: why Optional title
  markdown ...
  :::
      a box; kinds: why, kernel, cpu, measured, warn, try

Fenced code (```c, ```text, ```python) is highlighted the same way, without
line numbers. Raw HTML (figures with inline SVG) passes through.

python3 build.py [--check]  --check fails if a line of src/ is in no listing
"""
import html
import json
import pathlib
import re
import subprocess
import sys

import markdown
from pygments.lexers import get_lexer_by_name
from pygments.token import Token

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent  # docs/book in the repository
GITHUB = "https://github.com/martinus/quetschn/blob"


def git(*args):
    return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True,
                           check=True).stdout.strip()


# the commit of the last change to src/: the listings are src/ as it is in the checkout, and their
# links on GitHub point to that commit
SRC_COMMIT = git("log", "-1", "--format=%H", "--", "src")
OUT = HERE / "seqlz-line-by-line.html"

BOOK = json.loads((HERE / "book.json").read_text())

# token type -> css class
CLASSES = [
    (Token.Comment.Preproc, "pp"),
    (Token.Comment.PreprocFile, "pp"),
    (Token.Comment, "cm"),
    (Token.Keyword.Type, "ty"),
    (Token.Keyword, "kw"),
    (Token.Name.Function, "fn"),
    (Token.Name.Builtin, "bi"),
    (Token.Literal.String, "st"),
    (Token.Literal.Number, "nu"),
    (Token.Operator, "op"),
]
TYPES = {"u8", "u16", "u32", "u64", "size_t", "bool", "uint8_t", "uint16_t",
         "uint32_t", "uint64_t"}


def css_class(tok, val):
    if tok in Token.Name and val in TYPES:
        return "ty"
    for t, c in CLASSES:
        if tok in t:
            return c
    return None


def highlight_lines(text, lang):
    """list of html strings, one per line of text"""
    try:
        lexer = get_lexer_by_name(lang, stripnl=False, ensurenl=False, tabsize=0)
    except Exception:
        lexer = None
    if lexer is None or lang in ("text", "plain"):
        return [html.escape(l) for l in text.split("\n")]
    lines = [""]
    for tok, val in lexer.get_tokens(text):
        cls = css_class(tok, val)
        parts = val.split("\n")
        for i, part in enumerate(parts):
            if i > 0:
                lines.append("")
            if part:
                esc = html.escape(part)
                lines[-1] += f'<span class="{cls}">{esc}</span>' if cls else esc
    return lines


SRC_CACHE = {}


def src_lines(path):
    if path not in SRC_CACHE:
        SRC_CACHE[path] = (REPO / path).read_text().split("\n")
    return SRC_CACHE[path]


COVERED = {}  # path -> set of line numbers
LISTINGS = []


def render_listing(path, a, b):
    lines = src_lines(path)
    if not (1 <= a <= b <= len(lines)):
        sys.exit(f"listing {path} {a}-{b}: file has {len(lines)} lines")
    COVERED.setdefault(path, set()).update(range(a, b + 1))
    LISTINGS.append((path, a, b))
    lang = "c"
    body = "\n".join(lines[a - 1:b])
    hl = highlight_lines(body, lang)
    rows = "".join(
        f'<span class="line"><span class="ln" data-n="{a + i}"></span><span class="lc">{h}</span></span>'
        for i, h in enumerate(hl))
    rng = f"line {a}" if a == b else f"lines {a}&ndash;{b}"
    return (f'<figure class="listing" id="L-{path.replace("/", "-").replace(".", "-")}-{a}">'
            f'<figcaption><a class="lfile" href="{GITHUB}/{SRC_COMMIT}/{path}#L{a}-L{b}">{html.escape(path)}</a>'
            f'<span class="lrange">{rng}</span></figcaption>'
            f'<pre class="code numbered"><code>{rows}</code></pre></figure>')


def render_fence(lang, body):
    hl = highlight_lines(body, lang or "text")
    return (f'<pre class="code snippet" data-lang="{html.escape(lang or "")}"><code>'
            + "\n".join(hl) + "</code></pre>")


STASH = []


def stash(h):
    STASH.append(h)
    return f"\n\nXSTASHX{len(STASH) - 1}XSTASHX\n\n"


def unstash(h):
    h = re.sub(r"<p>XSTASHX(\d+)XSTASHX</p>", lambda m: STASH[int(m.group(1))], h)
    return re.sub(r"XSTASHX(\d+)XSTASHX", lambda m: STASH[int(m.group(1))], h)


BOX_TITLES = {"why": "Why", "kernel": "Kernel background", "cpu": "CPU background",
              "measured": "Measured", "warn": "Careful", "try": "Try it"}


def md(text):
    return markdown.markdown(text, extensions=["tables", "attr_list", "md_in_html", "sane_lists"],
                             output_format="html5")


def preprocess(text, where):
    out, i = [], 0
    lines = text.split("\n")
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^```(\w*)\s*$", line)
        if m:
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            if j == len(lines):
                sys.exit(f"{where}: unclosed fence at line {i + 1}")
            out.append(stash(render_fence(m.group(1), "\n".join(lines[i + 1:j]))))
            i = j + 1
            continue
        m = re.match(r"^@listing\s+(\S+)\s+(\d+)(?:-(\d+))?\s*$", line)
        if m:
            a = int(m.group(2))
            b = int(m.group(3) or a)
            out.append(stash(render_listing(m.group(1), a, b)))
            i += 1
            continue
        if line.startswith("@listing"):
            sys.exit(f"{where}:{i + 1}: bad listing line: {line}")
        m = re.match(r"^:::\s*(\w+)\s*(.*)$", line)
        if m:
            kind, title = m.group(1), m.group(2).strip()
            if kind not in BOX_TITLES:
                sys.exit(f"{where}:{i + 1}: unknown box kind {kind}")
            j, depth = i + 1, 1
            while j < len(lines):
                if re.match(r"^:::\s*\w", lines[j]):
                    depth += 1
                elif lines[j].strip() == ":::":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if j == len(lines):
                sys.exit(f"{where}:{i + 1}: unclosed box")
            inner = unstash(md(preprocess("\n".join(lines[i + 1:j]), where)))
            label = BOX_TITLES[kind]
            head = f'<span class="boxkind">{label}</span>'
            if title:
                head += f'<span class="boxtitle">{md(title)[3:-4]}</span>'
            out.append(stash(f'<aside class="box box-{kind}"><div class="boxhead">{head}</div>{inner}</aside>'))
            i = j + 1
            continue
        out.append(line)
        i += 1
    return "\n".join(out)


def slug(s):
    s = re.sub(r"<[^>]+>", "", s)
    s = re.sub(r"[^\w\s-]", "", s.lower())
    return re.sub(r"[\s]+", "-", s).strip("-")


def build():
    parts_html, toc = [], []
    ids = set()
    chap_no = 0
    for part in BOOK["parts"]:
        part_items = []
        if part.get("title"):
            parts_html.append(f'<div class="part" id="{part["id"]}"><span class="partno">{part["label"]}</span>'
                              f'<h1 class="parttitle">{part["title"]}</h1>'
                              f'<p class="partintro">{part.get("intro", "")}</p></div>')
        for ch in part["chapters"]:
            f = HERE / "chapters" / ch["file"]
            if not f.exists():
                parts_html.append(f'<section class="chapter missing"><h1>{ch["file"]} is not written yet</h1></section>')
                continue
            text = f.read_text()
            body = unstash(md(preprocess(text, ch["file"])))
            numbered = ch.get("numbered", True)
            if numbered:
                chap_no += 1
                label = f"Chapter {chap_no}"
            else:
                label = ch.get("label", "")
            # the first h1 is the chapter title
            m = re.search(r"<h1([^>]*)>(.*?)</h1>", body, re.S)
            if not m:
                sys.exit(f"{ch['file']}: no '# title'")
            idm = re.search(r'id="([^"]+)"', m.group(1))
            cid = idm.group(1) if idm else slug(m.group(2))
            title = m.group(2)
            body = (body[:m.start()]
                    + f'<header class="chapterhead"><span class="chapno">{label}</span>'
                      f'<h1 id="{cid}">{title}</h1></header>'
                    + body[m.end():])
            subs = []
            for hm in re.finditer(r"<h2([^>]*)>(.*?)</h2>", body, re.S):
                sid = re.search(r'id="([^"]+)"', hm.group(1))
                subs.append((sid.group(1) if sid else None, hm.group(2)))

            def add_id(hm):
                attrs, inner = hm.group(2), hm.group(3)
                if 'id="' in attrs:
                    return hm.group(0)
                sid = cid + "--" + slug(inner)
                return f"<{hm.group(1)} id=\"{sid}\"{attrs}>{inner}</{hm.group(1)}>"
            body = re.sub(r"<(h[234])([^>]*)>(.*?)</\1>", add_id, body, flags=re.S)
            for im in re.finditer(r'id="([^"]+)"', body):
                if im.group(1) in ids and not im.group(1).startswith("L-"):
                    print(f"warning: duplicate id {im.group(1)} in {ch['file']}")
                ids.add(im.group(1))
            subs = []
            for hm in re.finditer(r'<h2 id="([^"]+)"[^>]*>(.*?)</h2>', body, re.S):
                subs.append((hm.group(1), re.sub(r"<[^>]+>", "", hm.group(2))))
            parts_html.append(f'<section class="chapter" data-chapter="{cid}">{body}</section>')
            part_items.append((cid, label, re.sub(r"<[^>]+>", "", title), subs))
        toc.append((part, part_items))
    # links to ids that do not exist
    allhtml = "\n".join(parts_html)
    bad = sorted({m.group(1) for m in re.finditer(r'href="#([^"]+)"', allhtml)} - ids
                 - {p["id"] for p in BOOK["parts"] if "id" in p})
    for b in bad:
        print(f"warning: link to missing id #{b}")
    toc_html = []
    for part, items in toc:
        if part.get("title"):
            toc_html.append(f'<li class="tocpart"><a href="#{part["id"]}">{part["label"]}: {part["title"]}</a></li>')
        for cid, label, title, subs in items:
            sub = "".join(f'<li><a href="#{sid}">{html.escape(t)}</a></li>' for sid, t in subs)
            toc_html.append(f'<li class="tocchap" data-for="{cid}"><a href="#{cid}"><span class="tocno">{label}</span>'
                            f'<span class="toct">{html.escape(title)}</span></a>'
                            + (f'<ol class="tocsub">{sub}</ol>' if sub else "") + "</li>")
    commit = SRC_COMMIT[:7]
    if git("status", "--porcelain", "--", "src"):
        print("warning: src/ has changes that are not committed, the listings show them")
    tpl = (HERE / "template.html").read_text()
    out = (tpl.replace("{{TOC}}", "\n".join(toc_html))
              .replace("{{BODY}}", allhtml)
              .replace("{{COMMIT}}", commit))
    OUT.write_text(out)
    print(f"wrote {OUT}, {len(out) // 1024} KiB, {len(LISTINGS)} listings")


def check_coverage():
    missing_any = False
    for f in sorted((REPO / "src").iterdir()):
        rel = f"src/{f.name}"
        lines = src_lines(rel)
        if f.suffix == ".inc" or f.name.startswith("."):
            continue
        cov = COVERED.get(rel, set())
        miss = [i + 1 for i, l in enumerate(lines) if l.strip() and (i + 1) not in cov]
        if miss:
            missing_any = True
            # as ranges
            rngs, start = [], miss[0]
            for a, b in zip(miss, miss[1:] + [None]):
                if b != a + 1:
                    rngs.append(f"{start}-{a}" if a != start else f"{a}")
                    start = b
            print(f"not covered: {rel}: {', '.join(rngs)}")
    # overlaps are fine, but report listings that repeat many lines
    return not missing_any


if __name__ == "__main__":
    build()
    ok = check_coverage()
    if "--check" in sys.argv and not ok:
        sys.exit(1)
