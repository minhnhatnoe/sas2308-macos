#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Minh Nhat Nguyen
"""assemble.py TEMPLATE CMP.md FILLS.json OUT.md: insert compare.py tables and text fills."""
import sys, re, json
tpl = open(sys.argv[1]).read(); cmp = open(sys.argv[2]).read(); fills = json.load(open(sys.argv[3]))
sec = {m.group(1): m.group(2).strip() for m in re.finditer(r'### (\S+)\n\n(.*?)(?=\n### |\Z)', cmp, re.S)}
tpl = re.sub(r'<!--T:(\S+)-->', lambda m: sec.get(m.group(1), '(no data)'), tpl)
for k, v in fills.items(): tpl = tpl.replace(f'<!--{k}-->', v)
left = re.findall(r'<!--[A-Z:0-9]+-->', tpl)
if left: print('unfilled:', left, file=sys.stderr)
open(sys.argv[4], 'w').write(tpl)
