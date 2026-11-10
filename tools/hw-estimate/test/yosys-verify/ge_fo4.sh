#!/usr/bin/env bash
# Usage: bash ge_fo4.sh <design.v|.sv> <top> <liberty.lib> [out_dir]
# Env:   FLATTEN=1   flatten hierarchy (default 1; matches --hw-flatten-modules)
#        ADDER=ks    Kogge-Stone carry network (default: Yosys' own $lcu mapping)
#        RAW=1       also run the "raw" (unoptimized) flow (default 1; RAW=0 to skip)
# Runs the design through Yosys and reports:
#   area mode  : abc area-oriented mapping   -> GE (compare to your GE estimate)
#   speed mode : abc delay-oriented mapping  -> FO4 (compare to your FO4 estimate), + its GE cost
#   raw mode   : word-level structure kept, no cross-op optimization, no restructuring or
#                sizing in abc -> GE and FO4 from ONE netlist (closest to a word-level estimator)
set -euo pipefail
V=$1; TOP=$2; LIB=$(realpath "$3"); OUT=$(realpath -m "${4:-ys_out}")
FLATTEN=${FLATTEN:-1}; ADDER=${ADDER:-default}; RAW=${RAW:-1}
HERE=$(dirname "$(realpath "$0")")
mkdir -p "$OUT"
read NAND NAND_AREA FO4_PS < <(python3 "$HERE/libcal.py" "$LIB")
python3 "$HERE/libcal.py" "$LIB" --areas > "$OUT/areas.json"
printf 'set_driving_cell BUF_X1\nset_load 0.0\n' > "$OUT/abc.constr"

FL=""; [ "$FLATTEN" = 1 ] && FL="-flatten"
if [ "$ADDER" = ks ]; then
  SYNTH="synth $FL -top $TOP -run :fine; opt -fast -full; memory_map; opt -full; techmap -autoproc -map +/techmap.v -map $HERE/kogge_stone_lcu.v; opt -fast; synth -top $TOP -run check:"
  RAWMAP="techmap -autoproc -map +/techmap.v -map $HERE/kogge_stone_lcu.v"
else
  SYNTH="synth $FL -top $TOP"
  RAWMAP="techmap"
fi

# Raw flow: NO opt/wreduce/alumacc/share/abc -fast, so each Verilog operator is mapped on its own
# (a+b+c stays two adders, not one merged compressor tree). The abc script (abc_raw.script) does
# no dc2/dch restructuring and no buffering/sizing.
FLCMD=""; [ "$FLATTEN" = 1 ] && FLCMD="flatten;"
RAWSYNTH="proc; $FLCMD memory_collect; memory_map; $RAWMAP"

MODES="area speed"; [ "$RAW" = 1 ] && MODES="area speed raw"
for MODE in $MODES; do
  if [ "$MODE" = raw ]; then S="$RAWSYNTH"; else S="$SYNTH"; fi
  yosys -q -l "$OUT/yosys_$MODE.log" -p "
    read_verilog -sv $V
    hierarchy -check -top $TOP
    $S
    dfflibmap -liberty $LIB
    tee -o $OUT/abc_$MODE.txt abc -liberty $LIB -constr $OUT/abc.constr -script $HERE/abc_$MODE.script
    opt_clean -purge
    tee -q -o $OUT/stat_$MODE.json stat -json -liberty $LIB -top $TOP
    write_verilog -noattr $OUT/netlist_$MODE.v
  " >/dev/null 2>&1 || { echo "yosys failed, see $OUT/yosys_$MODE.log"; exit 1; }
done

python3 - "$OUT" "$NAND_AREA" "$FO4_PS" "$NAND" <<'PY'
import json, os, re, sys
out, nand_area, fo4_ps, nand = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
areas = json.load(open(f"{out}/areas.json"))
def ge(mode):
    top = next(iter(json.load(open(f"{out}/stat_{mode}.json"))["modules"].values()))
    logic = ff = 0.0; bits = 0
    for cell, n in top.get("num_cells_by_type", {}).items():
        a = areas.get(cell)
        if a is None: continue
        if re.search(r"DFF|LATCH", cell): ff += a*n; bits += n
        else: logic += a*n
    return logic/nand_area, ff/nand_area, bits
def delay(mode):
    ds = re.findall(r"Delay\s*=\s*([\d.]+)\s*ps", open(f"{out}/abc_{mode}.txt").read())
    return float(ds[-1]) if ds else float("nan")
def ratio(a, b): return f"{a/b:.2f}x" if b else "n/a"
la, fa, bits = ge("area"); ls, fs, _ = ge("speed"); d = delay("speed")
print(f"Units: 1 GE = {nand} = {nand_area} um^2 ; 1 FO4 = {fo4_ps:.2f} ps")
print(f"Logic GE (area mode):   {la:10.2f}")
print(f"FF GE:                  {fa:10.2f}  ({bits} flops, {fa/max(bits,1):.2f} GE/bit)")
print(f"Total GE (area mode):   {la+fa:10.2f}")
print(f"Critical path (speed):  {d:10.1f} ps = {d/fo4_ps:.2f} FO4   (comb only; no clk->Q/setup)")
print(f"  logic GE at that speed: {ls:.2f}")
print(f"")
if os.path.exists(f"{out}/stat_raw.json"):
    lr, fr, _ = ge("raw"); dr = delay("raw")
    print("--- raw (unoptimized): word-level structure kept, no restructuring or sizing ---")
    print(f"Raw logic GE:           {lr:10.2f}  ({ratio(lr, la)} area mode)")
    print(f"Raw FF GE:              {fr:10.2f}")
    print(f"Raw critical path:      {dr:10.1f} ps = {dr/fo4_ps:.2f} FO4  ({ratio(dr, d)} speed mode)")
PY
