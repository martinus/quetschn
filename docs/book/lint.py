import re, pathlib, sys
banned = r"\b(delve|leverage|meticulous(ly)?|seamless(ly)?|nuanced|myriad|testament|load-bearing|load bearing|utili[sz]e|facilitate|for example|for instance|let's|let us|dive|journey|under the hood|magic|blazing|elegant(ly)?|clever(ly)?|it's worth noting|worth noting)\b"
for f in sorted(pathlib.Path("chapters").glob("*.md")):
    infence = False
    for n, line in enumerate(f.read_text().split("\n"), 1):
        if line.startswith("```"): infence = not infence; continue
        if infence or line.startswith("<") and "svg" in line: continue
        prose = re.sub(r"`[^`]*`", "", line)
        hits = []
        if "—" in prose or "–" in prose: hits.append("dash")
        for m in re.finditer(banned, prose, re.I): hits.append(m.group(0))
        if hits: print(f"{f.name}:{n}: {hits}: {line.strip()[:110]}")
