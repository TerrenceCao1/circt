# Hardware Estimation Tool

A lightweight hardware cost estimation tool for CIRCT/MLIR designs.

'hw-estimate' takes in a CIRCT .mlir file and estimates the hardware resources required without running a full synthesis.

It currently reports:

- Gate Equivalence for:
  - Combinational logic
  - Flip-flops
  - SRAM

## Motivation

Hardware synthesis can be very expensive and time-consuming. This tool aims to provide a faster estimate of hardware cost, directly from CIRCT, making it useful for quickly comparing designs before synthesis. As well, CIRCT already has tools for compiling most HDLs into .mlir files, so this tool is also language agnostic!

## Build:

'hw-estimate' is built as part of CIRCT

### Prereqs

- CMake
- Ninja
- C++ Compiler

```bash
git clone https://github.com/TerrenceCao1/circt.git --recursive
cd circt

cmake -G Ninja llvm/llvm -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DLLVM_TARGETS_TO_BUILD=host \
    -DLLVM_ENABLE_PROJECTS=mlir \
    -DLLVM_EXTERNAL_PROJECTS=circt \
    -DLLVM_EXTERNAL_CIRCT_SOURCE_DIR=$PWD \
    -DLLVM_ENABLE_LLD=ON

ninja -C build check-circt
```

The resulting executable is:

```bash
build/bin/hw-estimate
```

## Usage:

```bash
# From the CIRCT root directory:
./build/bin/hw-estimate <input.mlir>

# For example:
./build/bin/hw-estimate test.mlir
```

The values used for estimation can be altered in the

```bash
tools/hw-estimate/cost-model.json
```

## TODO:

- Make the inputCostModelJSON parameter more modular (not a hard coded path as it is now)
- Add an array size threshold to differentiate if a firreg object should count for FF or SRAM (currently any array counts for Sram, when small arrays likely should count for FFs)
- Add FO4 Analysis