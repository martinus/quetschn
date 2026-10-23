#!/usr/bin/env python3
"""remap.py [<commit>] [--write]: the book brought up to the code of <commit>, HEAD if not given.
The book describes the code of one commit, "src" in book.json; this moves its @listing lines and the
line numbers its prose quotes ("line 24", "lines 21 to 26", "seqlz_decompress.c:17") from the files at
that commit to the files at <commit>. The old and the new file are matched line by line (difflib); a
number moves with its line, or with the nearest line that kept its text. --write also sets "src" to
<commit>, so a second run finds nothing to move.

It prints every change; a listing whose lines changed their text is marked CHANGED, and the prose
around it needs a look by hand. "line N" in the prose is taken as a line of the file of the
chapter's last listing; a sentence about cache lines or table rows that says "lines 64, 128" gets
moved too, so read the output before --write rewrites the chapters. Then build.py --check.
"""
import difflib
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
BOOK_JSON = ROOT / "docs" / "book" / "book.json"
args = [a for a in sys.argv[1:] if a != "--write"]
write = "--write" in sys.argv


def git(*a):
    return subprocess.run(["git", "-C", str(ROOT), *a], capture_output=True, text=True, check=True).stdout


old_rev = json.loads(BOOK_JSON.read_text())["src"]
new_rev = git("rev-parse", "--verify", f"{args[0] if args else 'HEAD'}^{{commit}}").strip()
maps, oldtext, newtext = {}, {}, {}


def exists(path):
    return subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e", f"{new_rev}:{path}"],
                          capture_output=True).returncode == 0


def linemap(path):
    if path not in maps:
        o = git("show", f"{old_rev}:{path}").split("\n")
        n = git("show", f"{new_rev}:{path}").split("\n")
        m = {}
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, o, n, autojunk=False).get_opcodes():
            if tag == "equal":
                for k in range(i2 - i1):
                    m[i1 + k + 1] = j1 + k + 1
        maps[path], oldtext[path], newtext[path] = m, o, n
    return maps[path]

def near(m, a, direction):
    """the new number of old line a, or of the nearest old line that kept its text"""
    for d in range(0, 40):
        for c in ((a + d, a - d) if direction > 0 else (a - d, a + d)):
            if c in m:
                return m[c] + (a - c)
    return None

for ch in sorted((ROOT / "docs/book/chapters").glob("*.md")):
    lines = ch.read_text().split("\n")
    cur = None  # the file of the last listing: what "line N" in the prose refers to
    changed = False
    for idx, line in enumerate(lines):
        m = re.match(r"^@listing\s+(\S+)\s+(\d+)(?:-(\d+))?\s*$", line)
        if m:
            path, a = m.group(1), int(m.group(2)); b = int(m.group(3) or a)
            cur = path
            mp = linemap(path)
            na, nb = near(mp, a, 1), near(mp, b, -1)
            same = all(k in mp for k in range(a, b + 1)) and mp[b] - mp[a] == b - a
            if (na, nb) != (a, b) or not same:
                tag = "" if same else "  CHANGED"
                print(f"{ch.name}:{idx+1}: @listing {path} {a}-{b} -> {na}-{nb}{tag}")
                lines[idx] = f"@listing {path} {na}-{nb}" if na != nb else f"@listing {path} {na}"
                changed = True
            continue
        # file:line references in the prose, e.g. src/seqlz_decompress.c:17 or seqlz_decompress.c:544
        def fileref(mm):
            path = mm.group(1) if mm.group(1).startswith("src/") else "src/" + mm.group(1)
            if not exists(path):
                return mm.group(0)
            mp = linemap(path)
            new = re.sub(r"\d+", lambda d: str(near(mp, int(d.group(0)), 1) or d.group(0)), mm.group(2))
            return mm.group(0).replace(mm.group(2), new)
        nl = re.sub(r"`?((?:src/)?[\w.]+\.(?:c|h)):(\d+(?:[-–]\d+)?)", fileref, line)
        # "line N" / "lines N to M" / "lines N and M": the file of the last listing
        if cur:
            mp = linemap(cur)
            def lineref(mm):
                # all numbers of the reference at once, so that a new number is not mapped again
                return re.sub(r"\d+", lambda d: str(near(mp, int(d.group(0)), 1) or d.group(0)), mm.group(0))
            nl = re.sub(r"\b[Ll]ines? \d+(?:(?:, | to | and |-|–)\d+)*", lineref, nl)
        if nl != line:
            print(f"{ch.name}:{idx+1}: {line.strip()[:110]}\n{'':>{len(ch.name)+len(str(idx+1))+2}}  -> {nl.strip()[:110]}")
            lines[idx] = nl
            changed = True
    if write and changed:
        ch.write_text("\n".join(lines))
if write:
    book = BOOK_JSON.read_text()
    BOOK_JSON.write_text(book.replace(f'"src": "{old_rev}"', f'"src": "{new_rev}"', 1))
    print(f"book.json: src {old_rev[:7]} -> {new_rev[:7]}")
