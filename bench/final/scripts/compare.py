#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
"""compare.py OLD_RESULTS.csv OLD_Q.csv NEW_RESULTS.csv -> markdown tables (before / now / delta) per pattern."""
import csv, sys, re
def load(p):
    d, jobs = {}, {}
    for r in csv.DictReader(open(p)):
        if r['job'] == 'ALL': d[r['label']] = r
        else: jobs.setdefault(r['label'], []).append(r)
    return d, jobs
old, oldj = load(sys.argv[1]); oq, _ = load(sys.argv[2]); new, newj = load(sys.argv[3])
# old A labels used this boot's disk names; map to serials
for lab, js in list(oldj.items()):
    m = re.match(r'(A-\w+-\w+)-(disk\d+)$', lab)
    if m and lab in old: old[m.group(1) + '-' + js[0]['name']] = old[lab]
for lab in list(old):
    if lab.startswith('K2-'): old['K-' + lab[3:]] = old[lab]       # K2 = valid flush runs
for lab in oq: old[lab] = oq[lab]
old['G-final2-1M-seq-q4'] = old.get('G-final-1M-seq-q4'); old['G-final2-64K-rand-q8'] = old.get('G-final-64K-rand-q8')
old['G-check-final2-1M'] = old.get('G-check-early-1M'); old['G-check-final2-64K'] = old.get('G-check-early-64K')
def ms(us): v = float(us) / 1000; return f'{v:.2f}' if v < 1 else (f'{v:.1f}' if v < 100 else f'{v:.0f}')
def f(x): return f'{float(x):.0f}' if float(x) >= 100 else f'{float(x):.1f}'
def row(lab):
    n = new.get(lab); o = old.get(lab)
    if not n: return None
    mb = lambda r: float(r['MBps']) if float(r['MBps']) else float(r['verify_w_MBps'] or 0) + float(r['verify_r_MBps'] or 0)
    if o:
        oi, ni = float(o['IOPS']), float(n['IOPS'])
        if oi == 0 and ni == 0: oi, ni = mb(o), mb(n)
        dl = (ni - oi) / oi * 100 if oi else 0
        flag = ' **' if abs(dl) > 5 else ''
        return f"| {lab} | {f(mb(o))} / {f(mb(n))} | {f(o['IOPS'])} / {f(n['IOPS'])} | {dl:+.1f} %{flag} | {ms(o['p50_us'])} / {ms(n['p50_us'])} | {ms(o['p99_us'])} / {ms(n['p99_us'])} | {ms(o['max_us'])} / {ms(n['max_us'])} | {n['errors']} / {n['mismatches']} |"
    return f"| {lab} | — / {f(mb(n))} | — / {f(n['IOPS'])} | new | — / {ms(n['p50_us'])} | — / {ms(n['p99_us'])} | — / {ms(n['max_us'])} | {n['errors']} / {n['mismatches']} |"
H = '| run | MB/s before / now | IOPS before / now | delta | p50 ms | p99 ms | max ms | err / mis (now) |\n|---|---|---|---|---|---|---|---|'
groups = [('A', r'^A-'), ('B', r'^B-'), ('C', r'^C-'), ('D', r'^D-'), ('E', r'^E-'), ('F1', r'^F-seqread'), ('F2', r'^F-rand'),
          ('K', r'^K-'), ('X', r'^X-'), ('Q', r'^Q-'), ('G', r'^G-'), ('H', r'^H-'), ('S', r'^S-')]
for g, rx in groups:
    labs = [l for l in new if re.match(rx, l)]
    if not labs: continue
    print(f'\n### {g}\n\n{H}')
    for l in labs: print(row(l))
