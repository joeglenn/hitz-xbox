"""Diff two hostprof_resolve.py sample dumps: keep only addresses whose count grew.

usage: hp_diff.py <before.txt> <after.txt> <out.txt>

Each input is a hostprof_win.c samples.txt (see hostprof_resolve.py's module
docstring for the format): a header line, then "mod" lines and sample lines
"<tid> <rip-hex> <count>". For every (tid, rip) pair present in "after",
subtracts the matching count from "before" (0 if absent there) and keeps
the line in out.txt only if the result is positive, so out.txt isolates
work that happened between the two captures (e.g. before/after a fix) while
dropping addresses whose sample count did not increase. "mod" lines are
copied through unchanged so hostprof_resolve.py can still resolve modules
from the diffed file. No functions: this is a short, one-shot script.
"""
import sys
a={}
L=open(sys.argv[1]).read().split("\n")
for l in L[1:]:
    p=l.split()
    if len(p)==3: a[(p[0],p[1])]=int(p[2])
B=open(sys.argv[2]).read().split("\n"); out=[B[0]]
for l in B[1:]:
    if l.startswith("mod "): out.append(l); continue
    p=l.split()
    if len(p)==3:
        d=int(p[2])-a.get((p[0],p[1]),0)
        if d>0: out.append("%s %s %d"%(p[0],p[1],d))
open(sys.argv[3],"w").write("\n".join(out)+"\n")
