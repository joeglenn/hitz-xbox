#!/usr/bin/env python3
"""rva2sym.py <exe.map> <rva-hex>... : resolve RVAs from [CRASH] output against an MSVC /MAP."""
import sys, re, bisect
mp = open(sys.argv[1], errors="replace").read().split("\n")
base = 0x140000000
syms = []
for l in mp:
    m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", l)
    if m: base = int(m.group(1), 16)
    m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s", l)
    if m: syms.append((int(m.group(2), 16) - base, m.group(1)))
syms.sort()
keys = [a for a, _ in syms]
for a in sys.argv[2:]:
    r = int(a, 16)
    i = bisect.bisect_right(keys, r) - 1
    print("0x%X  %s+0x%X" % (r, syms[i][1], r - syms[i][0]) if i >= 0 else "0x%X ?" % r)
