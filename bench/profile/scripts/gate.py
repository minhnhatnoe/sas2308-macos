import re,collections,sys
f=sys.argv[1]; ios=float(sys.argv[2]); HZ=4997
txt=open(f).read().split('=== kst\n',1)[1]
blocks=[];cur=[]
for l in txt.splitlines():
    t=l.strip()
    if not t: continue
    if re.fullmatch(r'\d+',t): blocks.append((int(t),cur)); cur=[]
    else: cur.append(t)
us=1e6/HZ/ios
GATE=re.compile(r'IOWorkLoop::runEventSources|IOCommandGate::runAction|IOCommandGate::runCommand|IOCommandGate::attemptAction|IOWorkLoop::runAction')
ACQ=re.compile(r'IOWorkLoop::closeGate|IORecursiveLockLock')
held=collections.Counter(); spin=collections.Counter(); entry=collections.Counter()
def short(x): return re.sub(r'\(.*','',x.replace('kernel.release.t8152`','K`').replace('com.apple.iokit.',''))[:80]
tot=0
for n,b in blocks:
    tot+=n
    if not b: continue
    gi=next((i for i,x in enumerate(b) if GATE.search(x)),None)
    if gi is None: continue
    ai=next((i for i,x in enumerate(b[:gi]) if ACQ.search(x)),None)
    # entry point: frame just below (caller of) the gate frame
    caller=short(b[gi+1]) if gi+1<len(b) else '-'
    key=short(b[gi])+' <- '+caller
    if ai is not None:
        # acquiring: is an outer gate frame above it? then the thread holds one and waits for another
        spin[key]+=n
    else:
        held[key]+=n
print('in-gate (gate held, holder on CPU): %.3f us/IO' % (sum(held.values())*us))
for k,n in held.most_common(12): print('  %.3f %s' % (n*us,k))
print('acquiring (spinning in closeGate): %.3f us/IO' % (sum(spin.values())*us))
for k,n in spin.most_common(8): print('  %.3f %s' % (n*us,k))
