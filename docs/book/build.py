#!/usr/bin/env python3
r"""Builds the book: chapters/*.md -> seqlz-line-by-line.html.

Directives, each on a line of its own, outside fenced code:

  @listing src/seqlz_match.h 13-22
      the lines 13 to 22 of that file, as they are in the repository, with their
      real line numbers and syntax highlighting

  ::: why Optional title
  markdown ...
  :::
      a box; kinds: why, kernel, cpu, measured, warn, try

  $$ tex $$
      a formula of its own, on a line of its own

  @anim <name> [wide] [facts=<prefix>,...] Caption
      an animation, anim/<name>.js on anim/core.js; if anim/<name>.c exists, it is built with src/
      and its output, JSON, is the animation's data. "wide" lets it use the width beside the column.
      facts= adds the facts under those prefixes to the data, as data.facts["ch02.cls.0.share"]

  @stepper decode | match
      the step-through of decode_block(), seqlz_decode()'s body, or seqlz_match_block(): stepper/instrument.py puts hooks
      into a copy of the real function, stepper/trace.c or trace_match.c records it on the pages,
      stepper/stepper.js or match.js plays it; needs a C compiler. The C programs of anim/ and
      stepper/ get their pages from pages.h

  ```c
  ... a whole program ...
  ```
  @program <name> [-D... flags] [with=dec_counted]
      the program above is built with src/ and run at build time; @output shows what it printed.
      with=dec_counted links src/seqlz_decompress.c with chapter 16's two counters instead

  @output <name> [lines]
      what the program <name> printed, as a text block; lines is e.g. 3-4 or 1-2,24-25

  {{key}}, {{key|grp}}
      a value the facts programs compute, anywhere in a chapter, also in code and figures: each
      facts/*.c is built with src/ and prints a JSON object, nested objects give dotted keys, list
      items their index (ab.bytes, tok.code.34). |grp writes a number in groups of 3 digits,
      524 912. An unknown key fails the build. A facts file with the line "// facts: also 16k" is
      built for 16 KiB pages too, its keys then start with k16.

  @assert <python expression>
      fails the build when false, for a sentence that depends on the code without quoting a
      number: the facts are its names (tok.len[0] == 0), and F("tok.len.0") reads a dotted key

$tex$ in a line is a formula in the text, outside code spans; \$ is a dollar sign. KaTeX renders
them at build time (math.mjs, `npm install` in docs/book first), so the page needs no script.

Fenced code (```c, ```text, ```python) is highlighted the same way, without
line numbers. Raw HTML (figures with inline SVG) passes through.

python3 build.py [--check] [--out DIR]
    --check fails if a line of src/ is in no listing, an id is twice on the page, or a link goes to
    an id that is not; --out writes the two pages into DIR instead of next to this script
"""
import base64
import html
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

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
OUT_DIR = pathlib.Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else HERE
OUT_DIR.mkdir(parents=True, exist_ok=True)
OUT = OUT_DIR / "seqlz-line-by-line.html"
STANDALONE = OUT_DIR / "seqlz-line-by-line.standalone.html"

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
FILE_COMMIT = {}
LANG = {".c": "c", ".h": "c", ".inc": "c", ".py": "python", ".sh": "bash", ".patch": "diff", ".cpp": "cpp"}


def file_commit(path):
    """the last commit that changed path, which its link on GitHub shows; src/ files share SRC_COMMIT"""
    if path.startswith("src/"):
        return SRC_COMMIT
    if path not in FILE_COMMIT:
        FILE_COMMIT[path] = git("log", "-1", "--format=%H", "--", path) or SRC_COMMIT
    return FILE_COMMIT[path]


def render_listing(path, a, b):
    lines = src_lines(path)
    if not (1 <= a <= b <= len(lines)):
        sys.exit(f"listing {path} {a}-{b}: file has {len(lines)} lines")
    COVERED.setdefault(path, set()).update(range(a, b + 1))
    LISTINGS.append((path, a, b))
    body = "\n".join(lines[a - 1:b])
    hl = highlight_lines(body, LANG.get(pathlib.Path(path).suffix, "text"))
    rows = "".join(
        f'<span class="line"><span class="ln" data-n="{a + i}"></span><span class="lc">{h}</span></span>'
        for i, h in enumerate(hl))
    rng = f"line {a}" if a == b else f"lines {a}&ndash;{b}"
    return (f'<figure class="listing" id="L-{path.replace("/", "-").replace(".", "-")}-{a}">'
            f'<figcaption><a class="lfile" href="{GITHUB}/{file_commit(path)}/{path}#L{a}-L{b}">{html.escape(path)}</a>'
            f'<span class="lrange">{rng}</span></figcaption>'
            f'<pre class="code numbered"><code>{rows}</code></pre></figure>')


# icons of the step-through's buttons, 24 x 24, drawn with the text colour
ICONS = {
    "first": '<path d="M6 5v14M18 6l-8 6 8 6z"/>',
    "prevseq": '<path d="M12 6l-7 6 7 6zM20 6l-7 6 7 6z"/>',
    "prev": '<path d="M15 6l-7 6 7 6"/>',
    "play": '<path class="i-play" d="M8 5l11 7-11 7z"/><path class="i-pause" d="M8 5v14M16 5v14"/>',
    "next": '<path d="M9 6l7 6-7 6"/>',
    "nextseq": '<path d="M4 6l7 6-7 6zM12 6l7 6-7 6z"/>',
}
STEP_BUTTONS = [("first", "To the start"), ("prevseq", "Previous sequence (shift + left)"),
                ("prev", "Step back (left)"), ("play", "Play or pause"), ("next", "Step (right)"),
                ("nextseq", "Next sequence (shift + right)")]


STEPPER_CSS = [False]
SRC_C = ("codes", "compress", "compress_literals", "decompress", "decompress_literals", "default_tables", "lit_sets")


def run_c(main, include_dirs=(), link=SRC_C, flags=(), extra=()):
    """main built with the codec of src/ and run, its stdout. A file of src/ that main #includes, to
    reach its static functions, is not linked a second time. Variables start at 0: the step-throughs
    record theirs before the function first sets them."""
    text = main.read_text()
    srcs = [str(REPO / f"src/seqlz_{n}.c") for n in link if f'#include "seqlz_{n}.c"' not in text]
    incs = [a for d in [*include_dirs, HERE, REPO / "src"] for a in ("-I", str(d))]
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["cc", "-O1", "-ftrivial-auto-var-init=zero", *flags, *incs, str(main), *srcs, *extra,
                        "-o", f"{tmp}/a"], check=True)
        return subprocess.run([f"{tmp}/a"], capture_output=True, text=True, check=True).stdout


# ---- programs, facts and asserts: what the text says that the code computes, computed at build time

def counted_decoder(path):
    """src/seqlz_decompress.c with two counters, n_fast and n_slow, and seqlz_decode() renamed to
    seqlz_decode_counted(): what the sed command of chapter 16 makes"""
    t = (REPO / "src" / "seqlz_decompress.c").read_text()
    for pat, sub in [(r'^#include "seqlz.h"$',
                      '#include "seqlz.h"\nunsigned int n_fast, n_slow;\n#define seqlz_decode seqlz_decode_counted'),
                     (r"^\t\t\ttok = next_token_fast\(", "\t\t\tn_fast++;\n\\g<0>"),
                     (r"^\t\t/\* the slow path: length values, and every check \*/$", "\\g<0>\n\t\tn_slow++;")]:
        t, n = re.subn(pat, sub, t, flags=re.M)
        if n != 1:
            sys.exit(f"counted_decoder: {pat} matched {n} times in src/seqlz_decompress.c")
    path.write_text(t)


def run_program(name, code, args, where):
    """a program of a chapter, built with src/ and run: its stdout"""
    flags = [a for a in args if a.startswith("-D")]
    unknown = [a for a in args if not a.startswith("-D") and a != "with=dec_counted"]
    if unknown:
        sys.exit(f"{where}: @program {name}: unknown {' '.join(unknown)}")
    with tempfile.TemporaryDirectory() as tmp:
        main = pathlib.Path(tmp) / f"{name}.c"
        main.write_text(code)
        extra = []
        if "with=dec_counted" in args:
            counted_decoder(pathlib.Path(tmp) / "dec_counted.c")
            extra.append(str(pathlib.Path(tmp) / "dec_counted.c"))
        try:
            return run_c(main, flags=flags, extra=extra)
        except subprocess.CalledProcessError as e:
            sys.exit(f"{where}: @program {name} failed: {e}")


def programs_of(text, where):
    """the chapter's ```c blocks with an @program line right after them: (name, code, args, where)"""
    lines, out = text.split("\n"), []
    for i, line in enumerate(lines):
        m = re.match(r"^@program\s+(\S+)(.*)$", line)
        if not m:
            continue
        if i == 0 or lines[i - 1] != "```":
            sys.exit(f"{where}:{i + 1}: @program must follow a ```c block")
        j = i - 2
        while j >= 0 and lines[j] != "```c":
            j -= 1
        if j < 0:
            sys.exit(f"{where}:{i + 1}: @program without its ```c block")
        out.append((m.group(1), "\n".join(lines[j + 1:i - 1]) + "\n", m.group(2).split(), f"{where}:{i + 1}"))
    return out


PROGRAMS = {}


def flatten(obj, prefix, out, where):
    if isinstance(obj, dict):
        for k, v in obj.items():
            flatten(v, f"{prefix}{k}.", out, where)
    elif isinstance(obj, list):
        out[prefix[:-1]] = obj
        for k, v in enumerate(obj):
            flatten(v, f"{prefix}{k}.", out, where)
    else:
        key = prefix[:-1]
        if key in out:
            sys.exit(f"{where}: fact {key} is defined twice")
        out[key] = obj


class Facts(dict):
    """the facts as names for @assert: tok.len[0] and ab.len for the keys tok.len.0 and ab.len"""

    def __getattr__(self, k):
        try:
            return self[k]
        except KeyError:
            raise AttributeError(k) from None

    def __getitem__(self, k):
        return super().__getitem__(str(k))


FACTS, FACT_TREE = {}, Facts()


def fact_tree():
    """the dotted keys as nested Facts; a key that is also the start of others stays a value there"""
    for key, v in FACTS.items():
        node, parts = FACT_TREE, key.split(".")
        for part in parts[:-1]:
            nxt = node.get(part)
            if not isinstance(nxt, Facts):
                nxt = node[part] = Facts()
            node = nxt
        if not isinstance(node.get(parts[-1]), Facts):
            dict.__setitem__(node, parts[-1], v)


def facts_of(c, flags=()):
    out = run_c(c, flags=flags)
    try:
        return json.loads(out)
    except json.JSONDecodeError as e:
        sys.exit(f"facts/{c.name}: not JSON: {e}")


def load_facts(pool):
    # what the repository has, which no program of src/ can print
    FACTS["src.lines"] = sum((REPO / "src" / f.name).read_text().count("\n") for f in (REPO / "src").iterdir()
                             if f.suffix in (".c", ".h"))
    files = sorted((HERE / "facts").glob("*.c"))
    # a facts file with the line "// facts: also 16k" is built a second time for 16 KiB pages, its
    # keys then start with k16.
    k16 = [c for c in files if "// facts: also 16k" in c.read_text()]
    runs = [(c, "", pool.submit(facts_of, c)) for c in files]
    runs += [(c, "k16.", pool.submit(facts_of, c, ("-DQUETSCHN_PAGE_BITS=14",))) for c in k16]
    for c, prefix, fut in runs:
        flatten(fut.result(), prefix, FACTS, f"facts/{c.name}")
    fact_tree()


def fact_text(v):
    if isinstance(v, list):
        return " ".join(str(x) for x in v)
    return str(v)


def substitute(text, where):
    """{{key}} and {{key|grp}} in a chapter replaced by the facts"""
    def one(m):
        key, fmt = m.group(1), m.group(2)
        if key not in FACTS:
            line = text[:m.start()].count("\n") + 1
            sys.exit(f"{where}:{line}: unknown fact {{{{{key}}}}}")
        v = FACTS[key]
        if fmt == "grp":
            return f"{v:,}".replace(",", " ")
        if fmt:
            sys.exit(f"{where}: unknown format |{fmt}")
        return fact_text(v)
    return re.sub(r"\{\{([A-Za-z0-9_.-]+)(?:\|(\w+))?\}\}", one, text)


def check_assert(expr, where):
    names = dict(FACT_TREE)
    names["F"] = lambda key: FACTS[key]
    try:
        ok = eval(expr, {"__builtins__": {"len": len, "sum": sum, "min": min, "max": max, "all": all,
                                          "any": any, "abs": abs, "sorted": sorted, "range": range,
                                          "int": int, "str": str, "round": round}}, names)
    except Exception as e:  # noqa: BLE001 the expression is the chapter's
        sys.exit(f"{where}: @assert {expr}: {type(e).__name__}: {e}")
    if not ok:
        FAILED.append(f"{where}: @assert {expr} is false; the sentence next to it needs a look")


FAILED = []  # the false asserts, all reported at the end of the build


def render_output(args, where):
    m = re.match(r"(\S+)(?:\s+([\d,-]+))?\s*$", args)
    if not m or m.group(1) not in PROGRAMS:
        sys.exit(f"{where}: @output <program> [lines]: no program {args.split()[0] if args else ''}")
    out = PROGRAMS[m.group(1)].result().rstrip("\n").split("\n")
    if m.group(2):
        sel = []
        for r in m.group(2).split(","):
            a, _, b = r.partition("-")
            a, b = int(a), int(b or a)
            if not 1 <= a <= b <= len(out):
                sys.exit(f"{where}: @output {args}: the program printed {len(out)} lines")
            sel += out[a - 1:b]
        out = sel
    return render_fence("text", "\n".join(out))


def run_trace(kind):
    """the trace of the real function on the step-through's pages, its file, first and last line"""
    k = STEPPERS[kind]
    with tempfile.TemporaryDirectory() as tmp:
        lines = subprocess.run([sys.executable, str(HERE / "stepper" / "instrument.py"), kind, str(REPO / k["src"]),
                                f"{tmp}/{k['copy']}"], capture_output=True, text=True, check=True).stdout.split()
        # the instrumented copy comes first on the include path; src/ files that would include it
        # instead of the original are not linked
        out = run_c(HERE / "stepper" / k["trace"], [tmp], k["link"])
    return out, k["src"], int(lines[0]), int(lines[1])


STEPPERS = {
    "decode": {
        "src": "src/seqlz_decompress.c", "copy": "dec_inst.c", "trace": "trace.c",
        "link": ("codes", "compress", "compress_literals", "decompress_literals", "default_tables", "lit_sets"),
        "pages": [("ab", "ab \u00d7 2048, 9 bytes"), ("slab", "chapter 16\u2019s page, 796 bytes")],
        "label": "decode_block() step by step", "fn": "decode_block()", "js": "stepper.js",
        "what": "decoding two pages", "seq": "a sequence",
    },
    "match": {
        "src": "src/seqlz_match.h", "copy": "seqlz_match.h", "trace": "trace_match.c", "link": (),
        "pages": [("pointers", "512 pointers"), ("slab", "chapter 16\u2019s page")],
        "label": "seqlz_match_block() step by step", "fn": "seqlz_match_block()", "js": "match.js",
        "what": "finding the sequences of two pages", "seq": "the next sequence emitted",
    },
}


def decode_panes():
    return (f'<section><h4>The bit reader&rsquo;s 64 bits</h4><div class="st-bits"></div><div class="st-regline"></div></section>'
            f'<section><h4>The variables</h4><div class="st-vars"></div>'
            f'<div class="st-legend">nl, len and off are set further down in each sequence; before that the copy shows 0, '
            f'the real function has whatever was on the stack</div></section>'
            f'<section><h4>The compressed page</h4><div class="st-bytes"></div>'
            f'<div class="st-legend">header, <span class="b-lit">literals</span>, <span class="b-bits">the sequences&rsquo; bitstream</span>; '
            f'the outlined byte is the next one the bit reader loads</div></section>'
            f'<section><h4 class="st-lithead"></h4><div class="st-litbytes"></div></section>'
            f'<section><h4>What this sequence costs</h4><div class="st-cost"></div></section>'
            f'<section><h4>The page, 4096 bytes</h4><div class="st-page"><canvas width="384" height="384" aria-hidden="true"></canvas></div>'
            f'<div class="st-window"></div>'
            f'<div class="st-legend">written by a <span class="k-lit">literal</span> or a <span class="k-match">match</span>; '
            f'outlined: what this step writes, and in the colour of a match where it copies from</div></section>')


def render_stepper(what, where):
    if what not in STEPPERS:
        sys.exit(f"{where}: unknown stepper {what}")
    k = STEPPERS[what]
    trace, path, a, b = PREBUILT[("stepper", what)].result() if ("stepper", what) in PREBUILT else run_trace(what)
    hl = highlight_lines("\n".join(src_lines(path)[a - 1:b]), "c")

    def only_comment(h):
        # a line with nothing but a comment, or blank: folded away while comments are hidden
        return not re.sub(r'<span class="cm">.*?</span>', "", h).strip()
    rows = "".join(f'<span class="line{" cmline" if only_comment(h) else ""}"><span class="ln" data-n="{a + i}"></span>'
                   f'<span class="lc">{h}</span></span>' for i, h in enumerate(hl))
    pages = "".join(f'<button type="button" data-page="{pid}" aria-pressed="false">{html.escape(t)}</button>'
                    for pid, t in k["pages"])
    buttons = "".join(f'<button type="button" class="icon" data-do="{b_}" title="{t}" aria-label="{t}">'
                      f'<svg viewBox="0 0 24 24" aria-hidden="true">{ICONS[b_]}</svg></button>' for b_, t in STEP_BUTTONS)
    sid = "stepper-" + what
    css = ""
    if not STEPPER_CSS[0]:
        STEPPER_CSS[0] = True
        css = (f'<style>{(HERE / "stepper" / "stepper.css").read_text()}</style>'
               f'<script>{(HERE / "stepper" / "frame.js").read_text()}</script>')
    return (css +
            f'<figure class="stepper nocomments" id="{sid}" data-kind="{what}" tabindex="-1" aria-label="{k["label"]}">'
            f'<div class="st-bar"><div class="st-pages" role="group" aria-label="Page">{pages}</div>'
            f'<div class="st-ctl">{buttons}</div>'
            f'<input type="range" class="st-scrub" min="0" value="0" aria-label="Step">'
            f'<span class="st-pos"></span>'
            f'<button type="button" class="st-cm" aria-pressed="false" title="Show the comment lines of the code">comments</button></div>'
            f'<p class="st-say" aria-live="polite"></p>'
            f'<div class="st-grid"><div class="st-code"><pre class="code numbered"><code>{rows}</code></pre></div>'
            f'<div class="st-state">{decode_panes() if what == "decode" else ""}</div></div>'
            f'<figcaption>The real <code>{k["fn"]}</code> of <code>{path}</code>, lines {a} to {b}, {k["what"]}: '
            f'a copy of the function with a hook in front of its statements recorded every step when this book was built. '
            f'The highlighted line is the one about to run. Keys: left and right step, with shift {k["seq"]}.</figcaption>'
            f'</figure>'
            f'<script type="application/json" id="{sid}-data">{trace}</script>'
            f'<script>{(HERE / "stepper" / k["js"]).read_text()}</script>')


ANIM_CORE = [False]


def anim_data(name):
    """the JSON that anim/<name>.c prints, built with the codec of src/, or None"""
    c = HERE / "anim" / f"{name}.c"
    if not c.exists():
        return None
    out = run_c(c)
    json.loads(out)  # fails here, not in the browser
    return out


# the builds of the figures' data, started all at once by build(), see prebuild()
PREBUILT = {}


def prebuild(texts):
    """starts the C builds that the chapters' @anim and @stepper lines need, in parallel: each
    compiles src/ again, and one after another they took 2 of the build's 3 seconds"""
    pool = ThreadPoolExecutor()
    for where, text in texts:
        for name, code, args, at in programs_of(text, where):
            if name in PROGRAMS:
                sys.exit(f"{at}: program {name} twice")
            PROGRAMS[name] = pool.submit(run_program, name, code, args, at)
    load_facts(pool)
    for where, text in texts:
        for m in re.finditer(r"^@anim\s+(\w[\w-]*)", text, re.M):
            PREBUILT.setdefault(("anim", m.group(1)), pool.submit(anim_data, m.group(1)))
        for m in re.finditer(r"^@stepper\s+(\w+)\s*$", text, re.M):
            if m.group(1) in STEPPERS:
                PREBUILT.setdefault(("stepper", m.group(1)), pool.submit(run_trace, m.group(1)))
    pool.shutdown(wait=False)


def render_anim(args, where):
    m = re.match(r"(\w[\w-]*)(\s+wide)?(?:\s+facts=(\S+))?\s*(.*)$", args)
    if not m:
        sys.exit(f"{where}: @anim <name> [wide] [facts=<prefix>,...] caption")
    name, wide, caption = m.group(1), bool(m.group(2)), m.group(4)
    d = HERE / "anim"
    if not (d / f"{name}.js").exists():
        sys.exit(f"{where}: anim/{name}.js does not exist")
    out = ""
    if not ANIM_CORE[0]:
        ANIM_CORE[0] = True
        out += f'<style>{(d / "core.css").read_text()}</style><script>{(d / "core.js").read_text()}</script>'
    if (d / f"{name}.css").exists():
        out += f'<style>{(d / f"{name}.css").read_text()}</style>'
    data = PREBUILT[("anim", name)].result() if ("anim", name) in PREBUILT else anim_data(name)
    if m.group(3):
        # the facts under these prefixes, as data.facts, so the figure shows the numbers the text does
        prefixes = m.group(3).split(",")
        picked = {k: v for k, v in FACTS.items() if any(k == p or k.startswith(p + ".") for p in prefixes)
                  and not isinstance(v, list)}
        if not picked:
            sys.exit(f"{where}: @anim {name}: no facts under {m.group(3)}")
        obj = json.loads(data) if data is not None else {}
        obj["facts"] = picked
        data = json.dumps(obj)
    fid = f"anim-{name}"
    out += (f'<figure class="anim{" wide" if wide else ""}" id="{fid}" data-anim="{name}" tabindex="-1">'
            f'<div class="an-bar"></div><p class="an-say" aria-live="polite"></p><div class="an-stage"></div>'
            + (f'<figcaption>{md(caption)[3:-4]}</figcaption>' if caption else "") + '</figure>')
    if data is not None:
        out += f'<script type="application/json" id="{fid}-data">{data}</script>'
    out += f'<script>{(d / f"{name}.js").read_text()}</script>'
    return out


def render_fence(lang, body):
    hl = highlight_lines(body, lang or "text")
    return (f'<pre class="code snippet" data-lang="{html.escape(lang or "")}"><code>'
            + "\n".join(hl) + "</code></pre>")


STASH = []


class Math:
    """KaTeX in one node process for the whole build, one formula per request"""

    def __init__(self):
        self.proc = None

    def render(self, tex, display, where):
        if self.proc is None:
            self.proc = subprocess.Popen(["node", str(HERE / "math.mjs")], cwd=HERE, text=True,
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.proc.stdin.write(json.dumps([tex, display]) + "\n")
        self.proc.stdin.flush()
        out = json.loads(self.proc.stdout.readline())
        if isinstance(out, dict):
            sys.exit(f"{where}: formula {tex!r}: {out['error']}")
        return out


MATH = Math()
# a code span, or $...$ that starts and ends next to a non-space and is not part of a word or number
CODE_SPAN = re.compile(r"(``.*?``|`[^`]*`)")
INLINE_MATH = re.compile(r"(?<![\\$\w])\$(?=\S)(.+?)(?<=[^\s\\])\$(?![\w$])")


def katex_css(page):
    """KaTeX's stylesheet with its fonts in it, woff2 only, and only the font families that page
    uses: all 20 were 282 KiB of base64, which does not compress, for 2 or 3 families in use"""
    dist = HERE / "node_modules" / "katex" / "dist"
    css = (dist / "katex.min.css").read_text()
    used_classes = set(c for attr in re.findall(r'class="([^"]+)"', page) for c in attr.split())
    # KaTeX_Main is the font of .katex itself; the others come with a class of the formula's spans
    used = {"KaTeX_Main"}
    for sel, body in re.findall(r"([^{}]+)\{([^{}]*font-family:[^{}]*)\}", css):
        fams = set(re.findall(r"KaTeX_\w+", body))
        if fams and set(re.findall(r"\.([\w-]+)", sel)) - {"katex"} & used_classes:
            used |= fams

    def face(m):
        fam = re.search(r'font-family:"?(KaTeX_\w+)', m.group(0)).group(1)  # SansSerif's is quoted
        if fam not in used:
            return ""
        return re.sub(r'src:url\(fonts/([\w-]+)\.woff2\) format\("woff2"\)[^;}]*',
                      lambda f: 'src:url(data:font/woff2;base64,'
                                + base64.b64encode((dist / "fonts" / f"{f.group(1)}.woff2").read_bytes()).decode()
                                + ') format("woff2")', m.group(0))
    return re.sub(r"@font-face\{[^}]*\}", face, css)



def math_line(line, where):
    """the formulas of one line of markdown, rendered and stashed"""
    m = re.fullmatch(r"\s*\$\$(.+)\$\$\s*", line)
    if m:
        return stash(MATH.render(m.group(1).strip(), True, where))
    parts = CODE_SPAN.split(line)
    for k in range(0, len(parts), 2):
        parts[k] = INLINE_MATH.sub(lambda m: stash_inline(MATH.render(m.group(1), False, where)), parts[k])
        if re.search(r"(?<!\\)\$", parts[k]):
            sys.exit(f"{where}: a $ without its pair; a formula has to be on one line, a dollar sign is \\$")
        parts[k] = parts[k].replace("\\$", "$")
    return "".join(parts)


def stash_inline(h):
    STASH.append(h)
    return f"XSTASHX{len(STASH) - 1}XSTASHX"


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


def preprocess(text, where, first=1):
    """first: the line of the chapter that text starts at, for the messages"""
    out, i = [], 0
    at = lambda k: f"{where}:{first + k}"  # noqa: E731
    lines = text.split("\n")
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^```(\w*)\s*$", line)
        if m:
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            if j == len(lines):
                sys.exit(f"{at(i)}: unclosed fence")
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
        m = re.match(r"^@anim\s+(.*)$", line)
        if m:
            out.append(stash(render_anim(m.group(1).strip(), at(i))))
            i += 1
            continue
        m = re.match(r"^@stepper\s+(\w+)\s*$", line)
        if m:
            out.append(stash(render_stepper(m.group(1), at(i))))
            i += 1
            continue
        if line.startswith("@program"):  # built by prebuild(), the code is shown by its fence
            i += 1
            continue
        m = re.match(r"^@output\s+(.*)$", line)
        if m:
            out.append(stash(render_output(m.group(1).strip(), at(i))))
            i += 1
            continue
        m = re.match(r"^@assert\s+(.+)$", line)
        if m:
            check_assert(m.group(1).strip(), at(i))
            i += 1
            continue
        if line.startswith("@listing"):
            sys.exit(f"{at(i)}: bad listing line: {line}")
        m = re.match(r"^:::\s*(\w+)\s*(.*)$", line)
        if m:
            kind, title = m.group(1), m.group(2).strip()
            if kind not in BOX_TITLES:
                sys.exit(f"{at(i)}: unknown box kind {kind}")
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
                sys.exit(f"{at(i)}: unclosed box")
            inner = unstash(md(preprocess("\n".join(lines[i + 1:j]), where, first + i + 1)))
            label = BOX_TITLES[kind]
            head = f'<span class="boxkind">{label}</span>'
            if title:
                head += f'<span class="boxtitle">{md(title)[3:-4]}</span>'
            out.append(stash(f'<aside class="box box-{kind}"><div class="boxhead">{head}</div>{inner}</aside>'))
            i = j + 1
            continue
        out.append(math_line(line, at(i)))
        i += 1
    return "\n".join(out)


def slug(s):
    s = re.sub(r"<[^>]+>", "", s)
    s = re.sub(r"[^\w\s-]", "", s.lower())
    return re.sub(r"[\s]+", "-", s).strip("-")


# what --check fails on besides lines of src/ that are in no listing
WARNINGS = []


def warn(msg):
    WARNINGS.append(msg)
    print(f"warning: {msg}")


def build():
    parts_html, toc = [], []
    prebuild([(ch["file"], (HERE / "chapters" / ch["file"]).read_text()) for part in BOOK["parts"]
              for ch in part["chapters"] if (HERE / "chapters" / ch["file"]).exists()])
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
            text = substitute(f.read_text(), ch["file"])
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

            def add_id(hm):
                attrs, inner = hm.group(2), hm.group(3)
                if 'id="' in attrs:
                    return hm.group(0)
                sid = cid + "--" + slug(inner)
                return f"<{hm.group(1)} id=\"{sid}\"{attrs}>{inner}</{hm.group(1)}>"
            body = re.sub(r"<(h[234])([^>]*)>(.*?)</\1>", add_id, body, flags=re.S)
            for im in re.finditer(r'id="([^"]+)"', body):
                if im.group(1) in ids and not im.group(1).startswith("L-"):
                    warn(f"duplicate id {im.group(1)} in {ch['file']}")
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
        warn(f"link to missing id #{b}")
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
    out = (tpl.replace("{{KATEX_CSS}}", katex_css(allhtml))
              .replace("{{TOC}}", "\n".join(toc_html))
              .replace("{{BODY}}", allhtml)
              .replace("{{COMMIT}}", commit))
    OUT.write_text(out)
    print(f"wrote {OUT}, {len(out) // 1024} KiB, {len(LISTINGS)} listings")
    # the same page for a browser without the artifact host: its own skeleton, and mermaid from a CDN
    head, body = out.split("</style>", 1)
    STANDALONE.write_text(
        '<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n'
        '<style>body { margin: 0; }</style>\n' + head + '</style>\n</head>\n<body>\n' + body
        + '\n<script src="https://cdn.jsdelivr.net/npm/mermaid@11.4.1/dist/mermaid.min.js"></script>\n'
        + MERMAID_SCRIPT + '</body>\n</html>\n')
    print(f"wrote {STANDALONE}")


# the charts drawn again in the other theme when the reader switches it, from their source
MERMAID_SCRIPT = """<script>
(function () {
  var charts = Array.prototype.slice.call(document.querySelectorAll('pre.mermaid'));
  var sources = charts.map(function (c) { return c.innerHTML; });
  var system = matchMedia('(prefers-color-scheme: dark)');
  function theme() {
    var t = document.documentElement.getAttribute('data-theme');
    return (t ? t === 'dark' : system.matches) ? 'dark' : 'neutral';
  }
  function draw() {
    charts.forEach(function (c, i) { c.removeAttribute('data-processed'); c.innerHTML = sources[i]; });
    mermaid.initialize({ startOnLoad: false, theme: theme() });
    mermaid.run({ nodes: charts });
  }
  document.addEventListener('book-theme', draw);
  system.addEventListener('change', draw);
  draw();
})();
</script>
"""


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
    return not missing_any


if __name__ == "__main__":
    build()
    if FAILED:
        print("\n".join(FAILED))
        sys.exit(f"{len(FAILED)} @assert false: read those paragraphs again, then fix them or the assert")
    ok = check_coverage()
    if "--check" in sys.argv and (not ok or WARNINGS):
        sys.exit(1)
