#!/usr/bin/env python3
"""Compare instruction counts per function: a hardware DPU ELF against a
uPIMulator linker run's main.S. A function whose length differs runs
different code on the two sides.

    python3 runner/scripts/rtdiff.py <ELF> <bin-dir>/main.S
"""
import re, subprocess, sys
elf, mains = sys.argv[1], sys.argv[2]
dis = subprocess.run(['/usr/bin/llvm-objdump', '-d', elf], capture_output=True, text=True).stdout
hw = {}; cur = None
for l in dis.split('\n'):
    m = re.match(r'^[0-9a-f]+ <([^>]+)>:', l)
    if m: cur = m.group(1); hw[cur] = []; continue
    m = re.match(r'^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} ){8}\s*(.*)$', l)
    if m and cur: hw[cur].append(m.group(1).strip())
sim = {}; cur = None
for l in open(mains):
    s = l.rstrip('\n')
    m = re.match(r'^([A-Za-z_][\w.$]*):', s)
    if m and not m.group(1).startswith('.') and '..' not in m.group(1):
        cur = m.group(1); sim.setdefault(cur, []); continue
    t = s.strip()
    if cur and t and not t.startswith(('.', '//')) and not re.match(r'^[\w.$]+:', t):
        sim[cur].append(t)
both = sorted(set(hw) & set(sim))
print(f"{'function':32s} {'hw':>5s} {'sim':>5s}")
for f in both:
    a, b = len(hw[f]), len(sim[f])
    if a != b: print(f"{f:32s} {a:5d} {b:5d}   DIFFERS")
same = sum(1 for f in both if len(hw[f]) == len(sim[f]))
print(f"{same} of {len(both)} common functions have the same length")
only_hw = sorted(f for f in hw if f not in sim and hw[f])
print("only in the hardware image:", ' '.join(only_hw[:40]))
