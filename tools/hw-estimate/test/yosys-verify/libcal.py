#!/usr/bin/env python3
"""Calibrate a Liberty file for GE / FO4 conversion.
  GE  unit = area of the 2-input NAND (smallest NAND2_* cell)
  FO4 unit = delay of an inverter driving 4 copies of its own input pin
Prints: NAND2_CELL NAND2_AREA FO4_PS   (units: library area, ps)
"""
import re, sys, bisect

txt = open(sys.argv[1]).read()
tu = re.search(r'time_unit\s*:\s*"?1(\w+)"?', txt)
to_ps = {'ps': 1.0, 'ns': 1000.0, 'fs': 0.001}[tu.group(1)] if tu else 1000.0

def cell_block(name):
    m = re.search(r'cell\s*\(\s*"?%s"?\s*\)\s*\{' % re.escape(name), txt)
    i, depth = m.end(), 1
    while depth:
        c = txt[i]; depth += (c == '{') - (c == '}'); i += 1
    return txt[m.start():i]

cells = re.findall(r'cell\s*\(\s*"?(\w+)"?\s*\)\s*\{', txt)
def area(n): return float(re.search(r'area\s*:\s*([\d.]+)', cell_block(n)).group(1))
if len(sys.argv) > 2 and sys.argv[2] == '--areas':
    import json; print(json.dumps({c: area(c) for c in cells})); sys.exit()
nands = sorted((c for c in cells if re.match(r'NAND2_X\d+$', c)), key=area)
nand = nands[0]

# smallest inverter
invs = sorted((c for c in cells if re.match(r'INV_X\d+$', c)), key=area)
inv = invs[0]; blk = cell_block(inv)
cin = float(re.search(r'pin\s*\(\s*"?A"?\s*\)\s*\{[^}]*?capacitance\s*:\s*([\d.]+)', blk, re.S).group(1))

def tables(kind):  # kind in cell_rise/cell_fall/rise_transition/fall_transition
    m = re.search(kind + r'\s*\(\s*"?\w+"?\s*\)\s*\{(.*?)\}', blk, re.S)
    b = m.group(1)
    nums = lambda k: [float(x) for x in re.search(k + r'\s*\(\s*"([^"]*)"', b).group(1).split(',')]
    i1, i2 = nums('index_1'), nums('index_2')   # slew, load
    vals = [[float(x) for x in r.split(',')] for r in re.findall(r'"([^"]*)"', re.search(r'values\s*\((.*?)\)\s*;', b, re.S).group(1))]
    return i1, i2, vals

def interp(t, s, c):
    i1, i2, v = t
    def seg(ax, x):
        j = min(max(bisect.bisect_right(ax, x) - 1, 0), len(ax) - 2)
        return j, (x - ax[j]) / (ax[j+1] - ax[j])
    a, fa = seg(i1, s); b, fb = seg(i2, c)
    lo = v[a][b]*(1-fb) + v[a][b+1]*fb
    hi = v[a+1][b]*(1-fb) + v[a+1][b+1]*fb
    return lo*(1-fa) + hi*fa

T = {k: tables(k) for k in ('cell_rise', 'cell_fall', 'rise_transition', 'fall_transition')}
load = 4 * cin
slew = 0.05
for _ in range(20):  # fixed-point on self-consistent slew
    slew = 0.5 * (interp(T['rise_transition'], slew, load) + interp(T['fall_transition'], slew, load))
d = 0.5 * (interp(T['cell_rise'], slew, load) + interp(T['cell_fall'], slew, load))
print(nand, area(nand), d * to_ps)
