import re, sys
lib, net = sys.argv[1], sys.argv[2]

# cell -> area (um^2) from the liberty file
area = {m.group(1): float(m.group(2)) for m in
        re.finditer(r'cell\s*\(\s*"?(\w+)"?\s*\)\s*\{[^{}]*?\barea\s*:\s*([\d.]+)',
                    open(lib).read())}
nand = area["NAND2_X1"]
print(f"NAND2_X1 area = {nand} um^2  (1 GE)")

# count cell instances in the mapped netlist
counts = {}
for m in re.finditer(r'^\s*(\w+)\s+\S+\s*\(', open(net).read(), re.M):
    if m.group(1) in area:
        counts[m.group(1)] = counts.get(m.group(1), 0) + 1

ff  = sum(n * area[c] for c, n in counts.items() if c.startswith("DFF"))
tot = sum(n * area[c] for c, n in counts.items())
print(f"FF bits    : {sum(n for c, n in counts.items() if c.startswith('DFF'))}")
print(f"Logic GE   : {(tot - ff) / nand:,.0f}")
print(f"FF GE      : {ff / nand:,.0f}")
print(f"Total GE   : {tot / nand:,.0f}")
