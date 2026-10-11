#!/usr/bin/env python3
"""remap.py <rev> [--write]: after a change of src/, the book's @listing lines and the line numbers
its prose quotes ("line 24", "lines 21 to 26", "seqlz_decompress.c:17"), moved from the files at <rev>,
the commit the book was written against, to the files as they are now. The old and the new file
are matched line by line (difflib); a number moves with its line, or with the nearest line that kept
its text.

It prints every change; a listing whose lines changed their text is marked CHANGED, and the prose
around it needs a look by hand. "line N" in the prose is taken as a line of the file of the
chapter's last listing; a sentence about cache lines or table rows that says "lines 64, 128" gets
moved too, so read the output before --write rewrites the chapters. Then build.py --check.
"""
import difflib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
old_rev, write = sys.argv[1], "--write" in sys.argv
maps, oldtext, newtext = {}, {}, {}


def linemap(path):
    if path not in maps:
        o = subprocess.run(["git", "-C", str(ROOT), "show", f"{old_rev}:{path}"], capture_output=True, text=True, check=True).stdout.split("\n")
        n = (ROOT / path).read_text().split("\n")
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
            if not (ROOT / path).exists():
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
