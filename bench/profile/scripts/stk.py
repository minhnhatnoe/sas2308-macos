import sys,re,collections
f=sys.argv[1]; pat=sys.argv[2]; k=int(sys.argv[3]) if len(sys.argv)>3 else 3
txt=open(f).read().split('=== kst\n',1)[1]
blocks=[]; cur=[]
for l in txt.splitlines():
    t=l.strip()
    if not t: continue
    if re.fullmatch(r'\d+',t): blocks.append((int(t),cur)); cur=[]
    else: cur.append(t)
sel=[(n,b) for n,b in blocks if b and re.search(pat,b[0])]
print('total',sum(n for n,b in sel),'stacks',len(sel))
for n,b in sorted(sel,reverse=True)[:k]:
    print(n); print('\n'.join('   '+x[:150] for x in b[:14]))
