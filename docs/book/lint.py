"""lint.py: dashes and banned words in the prose of the chapters, outside code; exits 1 on a hit"""
import re, pathlib, sys
banned = r"\b(delve|leverage|meticulous(ly)?|seamless(ly)?|nuanced|myriad|testament|load-bearing|load bearing|utili[sz]e|facilitate|for example|for instance|let's|let us|dive|journey|under the hood|magic|blazing|elegant(ly)?|clever(ly)?|it's worth noting|worth noting)\b"
found = 0
# the chapters next to this file, wherever it is run from
for f in sorted((pathlib.Path(__file__).resolve().parent / "chapters").glob("*.md")):
    infence = False
    for n, line in enumerate(f.read_text().split("\n"), 1):
        if line.startswith("```"): infence = not infence; continue
        if infence or line.startswith("<") and "svg" in line: continue
        prose = re.sub(r"`[^`]*`", "", line)
        hits = []
        if "—" in prose or "–" in prose: hits.append("dash")
        for m in re.finditer(banned, prose, re.I): hits.append(m.group(0))
        if hits:
            print(f"{f.name}:{n}: {hits}: {line.strip()[:110]}")
            found += 1
sys.exit(1 if found else 0)
