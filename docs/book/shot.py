#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""Screenshots of an animation of the built book, one per step, with headless Chrome.

    docs/book/shot.py <anim> [--steps 0,5,last] [--narrow] [--dark] [--out <dir>]

Needs docs/book/seqlz-line-by-line.standalone.html from build.py and google-chrome (or $CHROME).
Writes <out>/<anim>-<step>-<wide|narrow>-<light|dark>.png, the figure alone on the page with the
book's styles, and prints for each step whether the stage scrolls sideways, which a figure must not.
--narrow draws the figure 380 px wide, the layout of a phone: headless Chrome's window is at least
500 px wide, so a narrower window does not show it.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent

# shows the figure alone at the given width, at step `step`, then puts the stage's widths into <pre id=M>
SCRIPT = """<script>
window.addEventListener('load', function () { setTimeout(function () {
  var f = document.querySelector('figure.anim[data-anim="%(anim)s"]');
  if (!f) { document.title = 'no figure'; return; }
  document.documentElement.setAttribute('data-theme', '%(theme)s');
  for (var e = f; e && e !== document.documentElement; e = e.parentElement)
    Array.prototype.forEach.call(e.parentElement.children, function (c) {
      if (c !== e && !/^(STYLE|SCRIPT|LINK)$/.test(c.tagName)) c.style.display = 'none';
    });
  for (var p = f.parentElement; p && p !== document.body; p = p.parentElement) { p.style.margin = '0'; p.style.padding = '8px'; p.style.maxWidth = 'none'; }
  f.style.margin = '0'; f.style.width = '%(width)s';
  window.dispatchEvent(new Event('resize'));
  var s = f.querySelector('.an-scrub'), step = '%(step)s' === 'last' ? +s.max : +'%(step)s';
  s.value = step; s.dispatchEvent(new Event('input'));
  window.scrollTo(0, 0);
  var st = f.querySelector('.an-stage'), m = document.createElement('pre');
  m.id = 'M'; m.style.display = 'none';
  m.textContent = 'step ' + step + ' of ' + (+s.max + 1) + ', narrow ' + f.classList.contains('narrow') + ', stage ' + st.clientWidth + ' px, content ' + st.scrollWidth + ' px';
  document.body.appendChild(m);
}, 300); });
</script>"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("anim")
    ap.add_argument("--steps", default="0,last")
    ap.add_argument("--narrow", action="store_true")
    ap.add_argument("--dark", action="store_true")
    ap.add_argument("--out", default=".")
    a = ap.parse_args()
    book = HERE / "seqlz-line-by-line.standalone.html"
    if not book.exists():
        sys.exit(f"{book} is missing: run docs/book/build.py first")
    chrome = os.environ.get("CHROME", "google-chrome")
    html = book.read_text()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    mode, theme = ("narrow" if a.narrow else "wide"), ("dark" if a.dark else "light")
    window = "500,1400" if a.narrow else "1300,1400"
    bad = False
    with tempfile.TemporaryDirectory() as tmp:
        for step in a.steps.split(","):
            page = pathlib.Path(tmp) / "p.html"
            js = SCRIPT % {"anim": a.anim, "theme": theme, "step": step,
                           "width": "380px" if a.narrow else "calc(100vw - 40px)"}
            page.write_text(html.replace("</body>", js + "</body>"))
            base = [chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--window-size={window}",
                    "--virtual-time-budget=3000"]
            dom = subprocess.run(base + ["--dump-dom", page.as_uri()], capture_output=True, text=True).stdout
            m = re.search(r'<pre id="M"[^>]*>(.*?)</pre>', dom, re.S)
            if not m:
                sys.exit(f"{a.anim}: no such animation in the book, or the page did not load")
            png = out / f"{a.anim}-{step}-{mode}-{theme}.png"
            subprocess.run(base + [f"--screenshot={png}", page.as_uri()], capture_output=True, check=True)
            w = re.search(r"stage (\d+) px, content (\d+) px", m.group(1))
            over = int(w.group(2)) > int(w.group(1))
            bad |= over
            print(f"{png}: {m.group(1)}{', SCROLLS SIDEWAYS' if over else ''}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
