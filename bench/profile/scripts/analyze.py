#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
"""Summarise prof.d output: us of CPU per IO by component (leaf frame) and by path (stack contains).
usage: analyze.py dt-prof.txt syms.txt BASE IOS SECS [dis.txt]
BASE = our kext's load address (hex), IOS / SECS = IOs completed in the window and its length."""
import re, sys, bisect, collections

HZ = 4997
path, symf, base, ios, secs = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4]), float(sys.argv[5])

syms = []
for l in open(symf):
    p = l.split()
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
saddr = [a for a, _ in syms]
KEXT_LO, KEXT_HI = base + 0xc000, base + 0x32400


def oursym(a):
    off = a - base
    i = bisect.bisect_right(saddr, off) - 1
    return syms[i][1] if i >= 0 else '?'


def demangle_short(s):
    m = re.match(r'_+ZN(\d+)(\w+)', s)
    if not m:
        return s.lstrip('_')
    out, rest = [], s[s.index('ZN') + 2:]
    while rest and rest[0].isdigit():
        n = re.match(r'(\d+)', rest).group(1)
        rest = rest[len(n):]
        out.append(rest[:int(n)])
        rest = rest[int(n):]
    return '::'.join(out) if out else s


sec, data = None, collections.defaultdict(list)
stacks, cur = [], []
for line in open(path):
    line = line.rstrip('\n')
    if line.startswith('=== '):
        sec = line[4:]
        continue
    if sec == 'kst':
        t = line.strip()
        if not t:
            continue
        if re.fullmatch(r'\d+', t):
            stacks.append((cur, int(t)))
            cur = []
        else:
            cur.append(t)
    elif sec:
        if line.strip():
            data[sec].append(line)

allsamp = int(data['all'][0]); ksamp = int(data['kernel'][0])
ncpu = len(data['cpu'])
us_per = 1e6 / HZ / ios  # us of CPU per IO for one sample
print(f'window {secs:.1f} s, IOs {ios} = {ios/secs:.0f} IOPS; samples {allsamp} ({ncpu} cpus), kernel {ksamp}; 1 sample = {us_per*1000:.3f} ns/IO')


def frame_mod(f):
    if f.startswith('0x'):
        a = int(f, 16)
        return ('ours', oursym(a)) if KEXT_LO <= a < KEXT_HI else ('unknown', f)
    m = re.match(r'([^`]+)`([^+]+)', f)
    return (m.group(1), m.group(2)) if m else (f, f)


IDLE = re.compile(r'machine_idle|processor_idle|idle_thread|arm64_.*idle|cpu_idle|wfi|_enable_preemption_idle|ml_arm_sleep|cpu_sleep|.*_idle$|CLPC|wfe')
SCHED = re.compile(r'thread_|sched_|waitq|csw_|machine_switch|Switch_context|Call_continuation|thread_block|clear_wait|wakeup|_mtx|lck_|semaphore|turnstile|hw_lock|ml_set_interrupts|thread_invoke|thread_dispatch|ast_|timer_call|setrun|choose_processor|realtime|cpu_signal|processor_|ipi|IORecursiveLock|IOLock')
DART = re.compile(r'DART|IOMapper|IODMACommand|iovm|pmap|IOGeneralMemoryDescriptor|IOMemoryDescriptor|vm_|IOMultiMemory|ppl_|IOMD')
VFS = re.compile(r'spec_|physio|vn_|VNOP|dkreadwrite|dkstrategy|buf_|pread|unix_syscall|fp_|vfs_|kauth|sandbox|mac_|read_nocancel|uio_|fo_|preadv|dofileread|IOMediaBSDClient|dk')
INTR = re.compile(r'IOInterrupt|IOFilterInterrupt|AIC|aic_|fleh_|interrupt|ml_.*intr|sleh_|IOWorkLoop|IOEventSource|IOCommandGate|threadMain|runEventSources')


def classify(mod, fn):
    if mod == 'ours':
        return 'kext SAS2308HBA'
    if 'IOSCSIParallelFamily' in mod:
        return 'IOSCSIParallelFamily'
    if 'IOSCSIArchitectureModelFamily' in mod or 'IOSCSIBlockCommands' in mod:
        return 'SAM (IOSCSIArchitectureModelFamily/BlockCommands)'
    if 'IOStorageFamily' in mod:
        return 'IOStorageFamily'
    if 'PCI' in mod or 'Thunderbolt' in mod or 'PCIE' in mod.upper():
        return 'PCIe/Thunderbolt drivers'
    if 'DART' in mod or 'Mapper' in mod:
        return 'DART/IOMMU (mapping)'
    if mod in ('mach_kernel', 'kernel.release.t8152'):
        if IDLE.search(fn):
            return 'idle'
        if DART.search(fn):
            return 'kernel VM/IOMD/DMA mapping'
        if INTR.search(fn):
            return 'kernel interrupt/work loop'
        if SCHED.search(fn):
            return 'kernel scheduler/locks/wakeups'
        if VFS.search(fn):
            return 'kernel syscall/VFS/physio'
        return 'kernel other'
    if mod == 'unknown':
        return 'unknown module'
    return 'other kext: ' + mod


excl = collections.Counter(); exfn = collections.Counter(); oursfn = collections.Counter()
PATHS = [('submit (SendSCSICommand)', r'SendSCSICommand'),
         ('completion (CompleteParallelTask)', r'CompleteParallelTask'),
         ('work loop (IOWorkLoop/checkForWork)', r'IOWorkLoop|checkForWork|IOInterruptEventSource'),
         ('done thread_call', r'DoneThread|thread_call_thread|DoneCallBody'),
         ('pread syscall', r'pread|unix_syscall'),
         ('idle', r'idle')]
incl = collections.Counter()
nsamp = 0
for fr, n in stacks:
    nsamp += n
    if not fr:
        continue
    mod, fn = frame_mod(fr[0])
    c = classify(mod, fn)
    excl[c] += n
    exfn[(c, demangle_short(fn))] += n
    names = ' '.join((oursym(int(f, 16)) if f.startswith('0x') and KEXT_LO <= int(f, 16) < KEXT_HI else f) for f in fr)
    for pname, rx in PATHS:
        if re.search(rx, names):
            incl[pname] += n
    if any(frame_mod(f)[0] == 'ours' for f in fr):
        incl['any frame in our kext'] += n
print(f'stacks kept: {nsamp} of {ksamp} kernel samples ({100*nsamp/ksamp:.1f} %)')
usr = sum(int(l.rsplit(None, 1)[1]) for l in data['user'])
print(f'user-mode samples {usr} = {usr*us_per:.2f} us/IO')
print('\n## exclusive (leaf frame) by component: us CPU per IO')
for c, n in excl.most_common():
    print(f'{c:55s} {n:8d} {n*us_per:7.3f}')
print('\n## inclusive (frame anywhere in the stack): us CPU per IO')
for c, n in incl.most_common():
    print(f'{c:55s} {n:8d} {n*us_per:7.3f}')
print('\n## top leaf functions (non-idle)')
for (c, f), n in [x for x in exfn.most_common() if x[0][0] != 'idle'][:40]:
    print(f'{n*us_per:7.3f} {n:7d} {c[:28]:28s} {f[:90]}')
print('\n## our kext, PCs by symbol (all samples, from @ours)')
for l in data['ours']:
    a, n = l.split()
    oursfn[demangle_short(oursym(int(a, 16)))] += int(n)
tot = sum(oursfn.values())
print(f'total {tot} = {tot*us_per:.3f} us/IO')
for f, n in oursfn.most_common(30):
    print(f'{n*us_per:7.3f} {n:7d} {f}')
print('\n## kernel threads (top)')
for l in data['kthr'][::-1][:15]:
    p = l.rsplit(None, 2)
    print(f'{p[0]:20s} tid {p[1]:>8s} {int(p[2]):8d} {int(p[2])/HZ/secs*100:6.1f} % of a CPU')
