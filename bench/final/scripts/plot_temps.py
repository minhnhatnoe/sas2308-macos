#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
"""plot_temps.py DIR -> DIR/temps.png and DIR/soak-temps.txt (whole run: IOC, drives, SoC)."""
import csv, sys, datetime as dt
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
D = sys.argv[1]
def t(s, base=None):
    x = dt.datetime.strptime(s, '%H:%M:%S')
    if x.hour < 12: x += dt.timedelta(days=1)          # run crosses midnight
    return x
mon = [r for r in csv.DictReader(open(f'{D}/monitor.csv')) if r['IOC temperature (C)']]
drv = list(csv.DictReader(open(f'{D}/drivetemps.csv'))); soc = list(csv.DictReader(open(f'{D}/soc.csv')))
soak = list(csv.DictReader(open(f'{D}/soak.csv')))
fig, ax = plt.subplots(figsize=(10, 4.5))
ax.plot([t(r['time']) for r in mon], [int(r['IOC temperature (C)']) for r in mon], label='IOC (SAS2308)', color='#c0392b')
sers = sorted({r['serial'] for r in drv})
for i, s in enumerate(sers):
    rr = [r for r in drv if r['serial'] == s and r['temp_C']]
    ax.plot([t(r['time']) for r in rr], [int(r['temp_C']) for r in rr], lw=0.8, color=plt.cm.Blues(0.4 + 0.08 * i), label=s)
ax.plot([t(r['time']) for r in soc], [float(r['tdie_max']) for r in soc], color='#27ae60', label='SoC tdie max')
ph = {}
for r in soak: ph.setdefault(r['phase'], []).append(t(r['time']))
for p, c in (('idle', '#eeeeee'), ('load', '#fde2c4'), ('cool', '#ddeeff')):
    if p in ph: ax.axvspan(min(ph[p]), max(ph[p]), color=c, zorder=0)
ax.text(min(ph['load']), 25.5, 'soak load (21 min)', fontsize=8)
ax.set_ylabel('C'); ax.set_ylim(25, 65); ax.legend(fontsize=7, ncol=3, loc='upper left'); ax.grid(alpha=0.3)
ax.set_title('Temperatures, final run 2026-11-20 23:41 - 11-21 01:00 (perf suite, then soak)')
import matplotlib.dates as mdates; ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M')); fig.autofmt_xdate(); fig.tight_layout(); fig.savefig(f'{D}/temps.png', dpi=90)
with open(f'{D}/soak-temps.txt', 'w') as f:
    f.write('soak.csv every 30 s: IOC | drives (min-max) | SoC tdie max | phase\n')
    for r in soak:
        dv = [int(r[s]) for s in sers if r.get(s, 'NA') != 'NA']
        f.write(f"{r['time']} {r['phase']:4} IOC {r['ioc_C']} {'#' * (int(r['ioc_C']) - 50):12} drives {min(dv)}-{max(dv)} {'#' * (max(dv) - 25):10} SoC {r['tdie_max']}\n")
