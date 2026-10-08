#!/usr/bin/env python3
"""The symbol CRCs of the phone's own kernel, from its modules, for patch_versions.py.

    crcs.py <dir with the phone's .ko files> > phone-crcs.txt

Get the modules first, as root on the phone: cp /vendor/lib/modules/*.ko /data/local/tmp/mods/, then
adb pull /data/local/tmp/mods. Each module's __versions table has the CRC of every kernel symbol it uses,
and they all agree, so together they give the CRCs of the symbols that the phone's modules use.
"""
import glob
import os
import struct
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
crcs = {}
for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.ko"))):
    d = open(p, "rb").read()
    shoff = struct.unpack_from("<Q", d, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    sh = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    stroff = sh[shstrndx][4]
    for s in sh:
        if d[stroff + s[0] : d.index(b"\0", stroff + s[0])] != b"__versions":
            continue
        for o in range(s[4], s[4] + s[5], 64):
            crc = struct.unpack_from("<Q", d, o)[0]
            sym = d[o + 8 : o + 64].split(b"\0")[0].decode()
            if sym and crcs.setdefault(sym, crc) != crc:
                sys.exit(f"{p}: {sym} has CRC {crc:#x}, another module {crcs[sym]:#x}")
for sym in sorted(crcs):
    print(f"{sym} {crcs[sym]:#010x}")
