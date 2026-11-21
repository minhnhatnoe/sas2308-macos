import re,collections,sys,bisect
f=sys.argv[1]; ios=float(sys.argv[2]); base=int(sys.argv[3],16); symf=sys.argv[4]; HZ=4997
syms=sorted((int(p[0],16),p[2]) for p in (l.split() for l in open(symf)) if len(p)==3 and p[1] in 'tT')
sa=[a for a,_ in syms]
def ours(a):
    i=bisect.bisect_right(sa,a-base)-1; return syms[i][1] if i>=0 else '?'
def dem(s):
    m=re.search(r'ZN(.*)',s)
    if not m: return s.lstrip('_')
    r=m.group(1); out=[]
    while r and r[0].isdigit():
        n=re.match(r'\d+',r).group(0); r=r[len(n):]; out.append(r[:int(n)]); r=r[int(n):]
    return '::'.join(out)
LO,HI=base+0xc000,base+0x32400
def name(fr):
    if fr.startswith('0x'):
        a=int(fr,16)
        if LO<=a<HI: return 'OURS`'+dem(ours(a))
        return 'X`'+fr[:14]
    return re.sub(r'\(.*','',re.sub(r'\+0x[0-9a-f]+$','',fr)).replace('kernel.release.t8152','K').replace('com.apple.iokit.','').replace('com.apple.driver.','')
txt=open(f).read().split('=== kst\n',1)[1]
blocks=[];cur=[]
for l in txt.splitlines():
    t=l.strip()
    if not t: continue
    if re.fullmatch(r'\d+',t): blocks.append((int(t),cur)); cur=[]
    else: cur.append(t)
us=1e6/HZ/ios
sel=sys.argv[5] if len(sys.argv)>5 else 'IOWorkLoop::runEventSources'
def comp(nm):
    if nm.startswith('OURS`'): return 'kext'
    m=nm.split('`')[0]
    if m=='IOSCSIParallelFamily': return 'IOSCSIParallelFamily'
    if m in('IOSCSIArchitectureModelFamily','IOSCSIBlockCommandsDevice'): return 'SAM'
    if m=='IOStorageFamily': return 'IOStorageFamily'
    if m in('IODARTFamily','AppleGen3DART') or 'sptm' in nm or nm.startswith('X`0xfffffe001c'): return 'DART/SPTM'
    return 'kernel:'+nm[2:40] if m=='K' else m
# inclusive by "first interesting frame from leaf" = attribute leaf sample to the closest frame that is ours/family/SAM/IOStorage/DART, else kernel leaf
incl=collections.Counter(); leafc=collections.Counter(); oursf=collections.Counter(); mid=collections.Counter()
tot=0
for n,b in blocks:
    names=[name(x) for x in b]
    if not any(sel in x for x in names): continue
    if any('closeGate' in x or 'IORecursiveLockLock' in x for x in names[:names.index(next(x for x in names if sel in x))]):
        continue
    tot+=n
    leafc[comp(names[0])]+=n
    # owner component: nearest frame from the leaf that is not plain kernel
    own=next((comp(x) for x in names if not x.startswith('K`') and not x.startswith('X`0xfffffe004')),'kernel')
    incl[own]+=n
    o=next((x for x in names if x.startswith('OURS`')),None)
    if o: oursf[o]+=n
    # which top-level step of the completion: frame just above our kext / family
    path=' > '.join(x for x in reversed(names) if not x.startswith('X`0xfffffe004'))
    for key,rx in (('DMA complete/unmap (IODMACommand::complete)','IODMACommand::complete'),('CompleteParallelTask',r'CompleteParallelTask'),('wakeup/thread_wakeup','wakeup|thread_go|setrun'),('SAM completion','TaskCompletion|CommandCompleted|CompleteSCSITask'),('IOStorage completion','IOStorage|dkreadwritecompletion')):
        if re.search(rx,path): mid[key]+=n
print('samples %d = %.3f us/IO under %s (holder on CPU)'%(tot,tot*us,sel))
print('-- by owning component (nearest non-kernel frame):')
for k,n in incl.most_common(15): print('  %.3f %s'%(n*us,k))
print('-- by leaf component:')
for k,n in leafc.most_common(15): print('  %.3f %s'%(n*us,k))
print('-- inclusive steps:')
for k,n in mid.most_common(): print('  %.3f %s'%(n*us,k))
print('-- innermost frame of ours on stack (inclusive of callees):')
for k,n in oursf.most_common(15): print('  %.3f %s'%(n*us,k))
