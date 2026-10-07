#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""check-links.py: every relative link and #anchor in the repository's Markdown files must resolve.

The docs link into each other by heading anchors, and renaming a heading breaks them without anything
else failing. Anchors are made as GitHub makes them: the heading lowercased, everything but letters,
digits, `_`, `-` and spaces dropped, spaces turned into `-`, and `-1`, `-2`, ... for repeated headings.
Links inside code blocks and to http(s) are not checked. Prints each broken link, exits 1 if there is
one. For CI; needs git."""

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FENCE = re.compile(r"^```.*?^```", re.MULTILINE | re.DOTALL)
LINK = re.compile(r"\]\(([^)\s]+)\)|<img src=\"([^\"]+)\"")


def anchors(path, cache={}):
    """the anchors of the headings of a Markdown file"""
    if path not in cache:
        found, seen = set(), {}
        for line in FENCE.sub("", path.read_text()).splitlines():
            m = re.match(r"#{1,6}\s+(.*)", line)
            if m:
                slug = re.sub(r"[^\w\- ]", "", m.group(1).strip().lower()).replace(" ", "-")
                n = seen.get(slug, 0)
                seen[slug] = n + 1
                found.add(slug if n == 0 else f"{slug}-{n}")
        cache[path] = found
    return cache[path]


def main():
    files = subprocess.run(["git", "ls-files", "*.md"], cwd=REPO, check=True, capture_output=True, text=True)
    broken = links = 0
    for name in files.stdout.split():
        md = REPO / name
        for m in LINK.finditer(FENCE.sub("", md.read_text())):
            link = m.group(1) or m.group(2)
            if re.match(r"[a-z]+:", link):
                continue
            links += 1
            path, _, anchor = link.partition("#")
            target = (md.parent / path).resolve() if path else md
            if not target.exists():
                print(f"{name}: {link}: no such file")
                broken += 1
            elif anchor and target.suffix == ".md" and anchor not in anchors(target):
                print(f"{name}: {link}: no such heading")
                broken += 1
    print(f"{links} relative links, {broken} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
