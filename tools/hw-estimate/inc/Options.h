/**
 * @file Options.h
 * @brief Command-line flags that are shared across translation units.
 *
 * Only the flags that library code (cost-model loaders and analyses) needs to
 * see are exposed here. All other options stay private to hw-estimate.cpp.
 */

#pragma once

#include "llvm/Support/CommandLine.h"

/**
 * @brief The `-w` flag: print every warning raised during GE / FO4 analysis.
 *
 * Defined in src/hw-estimate.cpp. Read by loadGECostModel(), parseFO4Entry(),
 * runGEAnalysis() and runFO4Analysis() to decide whether to emit warnings.
 */
extern llvm::cl::opt<bool> warn;
