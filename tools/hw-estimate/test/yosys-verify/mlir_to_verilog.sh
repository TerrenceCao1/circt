#!/usr/bin/env bash
# Usage: bash mlir_to_verilog.sh design.mlir [out.sv]
# Lowers the same MLIR you feed hw-estimate to SystemVerilog for Yosys.
# NOTE: not tested here (no CIRCT in my sandbox) -- adjust pass names to your CIRCT version.
set -euo pipefail
IN=$1; OUT=${2:-${IN%.mlir}.sv}
circt-opt "$IN" \
  --hw-flatten-modules \
  --lower-seq-firmem --lower-seq-to-sv \
  --export-verilog -o /dev/null > "$OUT"
echo "wrote $OUT"
