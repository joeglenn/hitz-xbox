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
