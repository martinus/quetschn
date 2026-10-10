#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""samecode.py [--show] <old.dis> <new.dis> <label>: two disassemblies of objdump -dr, function by function.

Prints "same code: <label>", or "DIFFERENT <label>:" and the functions whose code differs, and exits 1
then. What moves when other code moves is taken out: addresses, branch targets outside the function,
the page address of an adrp, the offset of a relocation against .text, the padding after a function.
MAP="old=new ..." in the environment names functions renamed between the two. --show prints a diff of
each function that differs.
"""
import difflib
import os
import re
import sys


def functions(path, renames):
    # gcc's copies of a function keep its name before a suffix: .constprop.0, .cold, -0x10
    def rename(name):
        m = re.match(r"([^.+-]*)(.*)", name)
        return renames.get(m[1], m[1]) + m[2]

    out, cur = {}, None
    for line in open(path):
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = rename(m[1])
            out[cur] = []
            continue
        if cur is None or not line.strip() or "file format" in line or line.startswith("Disassembly"):
            continue
        ins = re.sub(r"^\s*[0-9a-f]+:\s*", "", line.rstrip())

        # a target: the address goes, the symbol stays, and the offset too inside the same function
        def target(m):
            sym = rename(m[1])
            return "<" + sym + ((m[2] or "") if sym == cur else "") + ">"

        ins = re.sub(r"(?:0x)?[0-9a-f]+ <([^>+-]+)([+-]0x[0-9a-f]+)?>", target, ins)
        ins = re.sub(r"<([^>+-]+)([+-]0x[0-9a-f]+)?>", target, ins)

        # relocations against .text carry a section offset, which moves with the code before
        def sym(s):
            m = re.match(r"(.*?)([-+]0x[0-9a-f]+)?$", s)
            base = m[1].removeprefix(".text.")
            base = rename(base)
            return base if base == ".text" else base + (m[2] or "")

        ins = re.sub(r"(R_\w+)\s+(\S+)", lambda m: m[1] + " " + sym(m[2]), ins)
        # adrp's operand is a page address that moves with the code; its relocation says what it loads
        ins = re.sub(r"^(adrp\s+\w+,).*", r"\1", ins)
        out[cur].append(ins)
    for f in out:
        while out[f] and re.match(r"^(nop\w*|int3|udf|\.\.\.)", out[f][-1]):
            out[f].pop()
    return out


def main():
    args = sys.argv[1:]
    show = args[:1] == ["--show"]
    if show:
        args = args[1:]
    if len(args) != 3:
        sys.exit("usage: [MAP='old=new ...'] samecode.py [--show] <old.dis> <new.dis> <label>")
    rename = dict(x.split("=") for x in os.environ.get("MAP", "").split())
    a, b = functions(args[0], rename), functions(args[1], {})
    diff = sorted(f for f in set(a) | set(b) if a.get(f) != b.get(f) and not f.startswith(".Ltmp"))
    if not diff:
        print(f"same code: {args[2]}")
        return
    print(f"DIFFERENT {args[2]}: " + ", ".join(
        f + ("" if f in a and f in b else " (only old)" if f in a else " (only new)") for f in diff))
    if show:
        for f in diff:
            sys.stdout.writelines(l + "\n" for l in difflib.unified_diff(
                a.get(f, []), b.get(f, []), f"old {f}", f"new {f}", lineterm=""))
    sys.exit(1)


main()
