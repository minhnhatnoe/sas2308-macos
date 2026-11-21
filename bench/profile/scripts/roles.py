#!/usr/bin/env python3
"""us of CPU per IO by role (submitting threads, work loop, interrupt, idle) and, within a role, by
component (leaf attributed to the nearest frame of a known component). usage:
roles.py dt-prof.txt syms.txt BASE IOS"""
import re, sys, bisect, collections
HZ = 4997
f, symf, base, ios = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), float(sys.argv[4])
syms = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in open(symf)) if len(p) == 3 and p[1] in 'tT')
sa = [a for a, _ in syms]
LO, HI = base + 0xc000, base + 0x32400


def dem(s):
    m = re.search(r'ZN(.*)', s)
    if not m:
        return s.lstrip('_')
    r, out = m.group(1), []
    while r and r[0].isdigit():
        n = re.match(r'\d+', r).group(0); r = r[len(n):]; out.append(r[:int(n)]); r = r[int(n):]
    return '::'.join(out)


def name(fr):
    if fr.startswith('0x'):
        a = int(fr, 16)
        if LO <= a < HI:
            off = a - base
            if off >= 0x31cc8:
                return 'OURS`auth stubs'
            return 'OURS`' + dem(syms[bisect.bisect_right(sa, off) - 1][1])
        return ('SPTM`' if a < 0xfffffe0040000000 else 'K`') + fr
    s = re.sub(r'\(.*', '', re.sub(r'\+0x[0-9a-f]+$', '', fr))
    return s.replace('kernel.release.t8152', 'K').replace('com.apple.iokit.', '').replace('com.apple.driver.', '')


txt = open(f).read().split('=== kst\n', 1)[1]
blocks, cur = [], []
for l in txt.splitlines():
    t = l.strip()
    if not t:
        continue
    if re.fullmatch(r'\d+', t):
        blocks.append((int(t), cur)); cur = []
    else:
        cur.append(t)
allk = int(open(f).read().split('=== kernel\n')[1].split()[0])
us = 1e6 / HZ / ios
IDLE_LEAF = ('K`0xfffffe0040fb5678', 'K`0xfffffe0040fc0d90')


def comp(names):
    """component of a sample: gate spin first (thread waits for the gate), then nearest known frame"""
    for x in names:
        if 'closeGate' in x or 'IORecursiveLockLock' in x:
            return 'spin on the work-loop gate'
        if not x.startswith('K`0x'):
            break
    for x in names:
        m = x.split('`')[0]
        if m == 'OURS':
            return 'kext SAS2308HBA'
        if m in ('IODARTFamily', 'AppleGen3DART', 'SPTM') or 'sptm' in x:
            return 'DART/SPTM (map/unmap)'
        if 'IODMACommand' in x or 'IOGeneralMemoryDescriptor::dmaCommandOperation' in x:
            return 'DART/SPTM (map/unmap)'
        if m == 'IOSCSIParallelFamily':
            return 'IOSCSIParallelFamily'
        if m in ('IOSCSIArchitectureModelFamily', 'IOSCSIBlockCommandsDevice'):
            return 'SAM'
        if m == 'IOStorageFamily':
            return 'IOStorageFamily'
        if re.search(r'IOGeneralMemoryDescriptor|IOMemoryDescriptor|wireVirtual|vm_', x):
            return 'kernel memory descriptor/wiring'
        if re.search(r'thread_block|thread_invoke|thread_wakeup|wakeup|setrun|Switch_context|machine_switch|sched|waitq|turnstile', x):
            return 'kernel scheduler/wakeups'
    return 'kernel other (syscall, VFS, unsymbolised)'


def role(names, raw):
    j = ' '.join(names)
    if names and names[0] in IDLE_LEAF:
        return 'idle'
    if 'IOWorkLoop::threadMain' in j or 'runEventSources' in j:
        return 'work loop thread (completion)'
    if 'spec_read' in j or 'dkread' in j or 'SendSCSICommand' in j:
        return 'submitting threads (pread)'
    if re.search(r'Interrupt|fleh|sleh|aic', j):
        return 'interrupt context'
    return 'other kernel'


R = collections.defaultdict(collections.Counter)
tot = 0
for n, b in blocks:
    if not b:
        continue
    names = [name(x) for x in b]
    r = role(names, b)
    R[r][comp(names) if r != 'idle' else 'idle'] += n
    tot += n
print(f'IOs {ios:.0f}; kernel samples {allk}, in stacks {tot}; 1 sample = {us*1000:.4f} ns/IO')
for r, c in sorted(R.items(), key=lambda kv: -sum(kv[1].values())):
    s = sum(c.values())
    print(f'\n{r}: {s*us:.3f} us/IO')
    for k, n in c.most_common():
        if n * us >= 0.005:
            print(f'   {n*us:7.3f}  {k}')
