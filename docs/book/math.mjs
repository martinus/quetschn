// Renders the book's formulas for build.py: one JSON request per line on stdin, [tex, display],
// one JSON line on stdout, the HTML or {"error": message}.
import katex from "katex";
import readline from "node:readline";

const rl = readline.createInterface({ input: process.stdin });
rl.on("line", (line) => {
  const [tex, display] = JSON.parse(line);
  let out;
  try {
    out = katex.renderToString(tex, { displayMode: display, throwOnError: true, output: "htmlAndMathml" });
  } catch (e) {
    out = { error: String(e.message || e) };
  }
  process.stdout.write(JSON.stringify(out) + "\n");
});
