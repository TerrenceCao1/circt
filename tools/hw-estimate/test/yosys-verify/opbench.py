#!/usr/bin/env python3
"""Per-operator calibration: synthesize each op in isolation with Yosys and compare
against cost-model.json.   usage: opbench.py cost-model.json liberty.lib [widths...]
Models assumed (edit `model_ge` / `model_fo4` if your estimator differs):
  ge   = ge_per_bit*w + fixed            | mul/div: ge_quadratic_k * w^2
  fo4  = log2_coeff*log2(w)+log2_const   | div: fo4_per_bit*w + fo4_fixed | flat: fo4
"""
import json, math, os, subprocess, sys, tempfile
cm, lib = json.load(open(sys.argv[1])), sys.argv[2]
widths = [int(x) for x in sys.argv[3:]] or [8, 16, 32, 64]
here = os.path.dirname(os.path.abspath(__file__))

# name -> (cost-model key, verilog expr, [icmp group])
OPS = {
 "and":  ("comb.and", "a & b"),   "or":  ("comb.or", "a | b"),   "xor": ("comb.xor", "a ^ b"),
 "mux":  ("comb.mux", "s ? a : b"),
 "add":  ("comb.add", "a + b"),   "sub": ("comb.sub", "a - b"),
 "mul":  ("comb.mul", "a * b"),
 "divu": ("comb.divu", "a / b"),  "divs": ("comb.divs", "$signed(a) / $signed(b)"),
 "icmp_eq":  ("comb.icmp.eq",  "a == b"),
 "icmp_ult": ("comb.icmp.mag", "a < b"),
 "icmp_slt": ("comb.icmp.mag", "$signed(a) < $signed(b)"),
}
def entry(section, key):
    e = cm[section]
    if key.startswith("comb.icmp."):
        return e["comb.icmp"]["eq_predicates" if key.endswith("eq") else "mag_predicates"]
    return e[key]
def model_ge(key, w):
    e = entry("logic_ge", key)
    if "ge_quadratic_k" in e: return e["ge_quadratic_k"] * w * w
    return e.get("ge_per_bit", 0) * w + e.get("fixed_ge", 0)
def model_fo4(key, w):
    e = entry("delay_fo4", key)
    if "fo4_log2_coeff" in e: return e["fo4_log2_coeff"] * math.log2(w) + e.get("fo4_log2_const", 0)
    if "fo4_per_bit" in e: return e["fo4_per_bit"] * w + e.get("fo4_fixed", 0)
    return e.get("fo4", 0)

print(f"{'op':9}{'w':>4} | {'GE model':>10}{'GE yosys':>10}{'ratio':>7} | {'FO4 model':>10}{'FO4 yosys':>10}{'ratio':>7}")
for name, (key, expr) in OPS.items():
    for w in widths:
        if name.startswith("div") and w > 32: continue
        outw = 1 if name.startswith("icmp") else w
        v = (f"module top(input [{w-1}:0] a, b, input s, output [{outw-1}:0] y);\n"
             f"  assign y = {expr};\nendmodule\n")
        with tempfile.TemporaryDirectory() as d:
            open(f"{d}/t.v", "w").write(v)
            r = subprocess.run(["bash", f"{here}/ge_fo4.sh", f"{d}/t.v", "top", lib, f"{d}/o"],
                               capture_output=True, text=True)
        if r.returncode: print(f"{name:9}{w:4} | FAILED"); continue
        L = r.stdout
        ge = float(next(l for l in L.splitlines() if l.startswith("Logic GE")).split()[-1])
        fo = float(next(l for l in L.splitlines() if l.startswith("Critical")).split("=")[1].split("FO4")[0])
        mg, mf = model_ge(key, w), model_fo4(key, w)
        rg = f"{ge/mg:7.2f}" if mg else "    n/a"; rf = f"{fo/mf:7.2f}" if mf else "    n/a"
        print(f"{name:9}{w:4} | {mg:10.1f}{ge:10.1f}{rg} | {mf:10.2f}{fo:10.2f}{rf}")
