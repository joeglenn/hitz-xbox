#!/usr/bin/env python3
"""Resolve a hostprof_win.c sample file against an MSVC linker .map.

usage: hostprof_resolve.py <samples.txt> <exe.map> [top_n]
Prints per-thread totals, then the hottest functions overall and per busy thread.
"""
import bisect, collections, re, sys

samples, mapf = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

pref_base = 0x140000000
syms = []
for line in open(mapf, errors="replace"):
    m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
    if m:
        pref_base = int(m.group(1), 16)
        continue
    # " 0001:00012340       sub_00012340   0000000140013340 f   recomp_0001.obj"
    m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\s+(f\s+)?(?:i\s+)?(\S+)?", line)
    if m:
        addr = int(m.group(2), 16)
        if addr >= pref_base:
            syms.append((addr - pref_base, m.group(1), m.group(4) or ""))
syms.sort()
addrs = [s[0] for s in syms]

def name(rva):
    i = bisect.bisect_right(addrs, rva) - 1
    if i < 0:
        return "?"
    return syms[i][1]

lines = open(samples).read().split("\n")
base = int(lines[0].split()[1], 16)
mods = []
for l in lines:
    if l.startswith("mod "):
        _, b0, sz, nm = l.split(" ", 3)
        mods.append((int(b0, 16), int(sz, 16), nm))
def modname(rip):
    for b0, sz, nm in mods:
        if b0 <= rip < b0 + sz:
            return "<%s>" % nm
    return "<outside exe %x>" % (rip >> 16 << 16)
per_t = collections.Counter()
fn_all = collections.Counter()
fn_t = collections.defaultdict(collections.Counter)
outside = collections.Counter()
for l in lines[1:]:
    p = l.split()
    if len(p) != 3:
        continue
    tid, rip, n = int(p[0]), int(p[1], 16), int(p[2])
    per_t[tid] += n
    via = rip >> 63
    rip &= (1 << 63) - 1
    rva = rip - base
    if 0 <= rva < 0x10000000:
        nm = name(rva)
        if via and (nm.startswith("??_C@") or nm.startswith("s_") or nm.startswith("g_")):
            nm = "<data>"
        f = ("(wait) in " if via else "") + nm
    else:
        f = modname(rip)
    fn_all[f] += n
    fn_t[tid][f] += n

tot = sum(per_t.values())
print("symbols %d, samples %d" % (len(syms), tot))
print("== threads")
for t, n in per_t.most_common(12):
    print("  tid %6d %7d (%.1f%%)  top: %s" % (t, n, 100.0 * n / tot,
          ", ".join("%s %d" % kv for kv in fn_t[t].most_common(3))))
print("== hottest functions (all threads)")
for f, n in fn_all.most_common(top):
    print("  %7d %5.1f%%  %s" % (n, 100.0 * n / tot, f))
import os
_want = [int(x) for x in os.environ.get("HP_TIDS", "").split(",") if x]
for t, _ in (per_t.most_common(3) if not _want else [(x, 0) for x in _want]):
    tt = per_t[t]
    print("== tid %d (%d samples)" % (t, tt))
    for f, n in fn_t[t].most_common(int(os.environ.get("HP_N", "15"))):
        print("  %7d %5.1f%%  %s" % (n, 100.0 * n / tt, f))
