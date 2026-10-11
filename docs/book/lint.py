"""lint.py: dashes and banned words in the prose of the chapters, outside code, and measurements without a\ndate or a link; exits 1 on a hit"""
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
# A measured number goes stale when the code changes, so it says when or where it was measured: a link,
# explored-designs, or a date in its paragraph. A paragraph whose numbers are background, not this
# project's measurements, says so with <!-- undated: why -->.
UNITS = r"\d[\d.,\u202f ]*\s?(µs|ns\b|cycles\b|instructions per|bytes per page|bytes per µs|GHz|% (smaller|larger|faster|slower|less|more))"
DATED = (r"\]\(|explored-designs|<!-- undated: \S|\b\d{1,2}(st|nd|rd|th) (January|February|March|April|May|June|July|"
         r"August|September|October|November|December)\b|\b(September|October|November|December) 2026\b")
for f in sorted((pathlib.Path(__file__).resolve().parent / "chapters").glob("*.md")):
    text = re.sub(r"```.*?```", lambda m: "\n" * m.group(0).count("\n"), f.read_text(), flags=re.S)
    n = 1
    for para in re.split(r"(\n\s*\n)", text):
        if para.strip() and not para.lstrip().startswith("<") and re.search(UNITS, para) and not re.search(DATED, para):
            print(f"{f.name}:{n}: a measurement without a date or a link: {para.strip()[:90]}")
            found += 1
        n += para.count("\n")
sys.exit(1 if found else 0)
