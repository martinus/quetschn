#!/usr/bin/env python3
"""Gives modules built for the Mi 9T the phone's symbol CRCs.

    patch_versions.py <phone-crcs.txt> <Module.symvers> <.ko file> ...

The modules are built against phoenix-r-oss, not the phone's kernel, whose source Xiaomi did not publish,
and only a third of their CRCs match. In each module's __versions table a symbol that one of the phone's
own modules uses gets the phone's CRC, a kernel symbol that cannot be checked loses its entry (4.14 then
only warns), and symbols between these modules stay. Only for the test phone.
"""
import os
import struct
import sys

from crcs import versions

if len(sys.argv) < 4:
    sys.exit(__doc__)
phone = {l.split()[0]: int(l.split()[1], 16) for l in open(sys.argv[1])}
vmlinux = {l.split("\t")[1] for l in open(sys.argv[2]) if l.split("\t")[2] == "vmlinux"}
for p in sys.argv[3:]:
    d = bytearray(open(p, "rb").read())
    for o, sym in list(versions(d)):
        if sym in phone:
            struct.pack_into("<Q", d, o, phone[sym])
            what = "phone crc"
        elif sym in vmlinux:
            d[o + 8 : o + 64] = b"\0" * 56
            what = "removed, not verifiable"
        else:
            what = "kept, between these modules"
        print(f"{os.path.basename(p):20s} {sym:28s} {what}")
    open(p, "wb").write(d)
