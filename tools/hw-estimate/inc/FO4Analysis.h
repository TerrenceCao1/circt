/**
 * @file FO4Analysis.h
 * @brief Fanout-of-4 (critical path) delay analysis.
 */

#pragma once

#include <string>
#include <vector>
#include "CostModel.h"
#include "mlir/IR/Operation.h"
#include "llvm/Support/raw_ostream.h"

/**
 * @brief Results produced by runFO4Analysis().
 */
struct FO4Result
{
  double                    criticalFO4{};    ///< Delay of the longest timing path, in FO4.
  long long                 numTimingPaths{}; ///< Number of timing endpoints checked.
  std::string               criticalSinkName; ///< Name of the endpoint that ends the critical path.
  std::vector<std::string>  criticalPath;     ///< Critical path hops, startpoint first. Each entry reads "<op> @ <arrival> FO4".
};

/**
 * @brief Finds the critical combinational path under @p root, in FO4 units.
 *
 * Arrival times are propagated through the combinational logic: an operation's
 * output arrives at the latest input arrival plus the operation's own delay
 * from CostModel::delayFO4. Variadic operations whose entry has
 * FO4Cost::scalesWithOperands are scaled by `ceil(log2(numOperands))`.
 *
 * Path startpoints and their arrival times:
 *  - module inputs (block arguments): 0
 *  - `seq.firreg` outputs: CostModel::clkToQFO4
 *  - `hw.instance` and `llhd.process` outputs: 0
 *
 * Path endpoints:
 *  - the next-state input of each `seq.firreg` (CostModel::setupFO4 is added)
 *  - the operands of each `hw.instance`
 *  - the operands of `hw.output`
 *
 * The result is the largest endpoint delay. Operations missing from the
 * delay table contribute 0 delay. Combinational loops are cut where they are
 * detected, so the path through a loop may be wrong.
 *
 * With @p verbose, each operation's arrival time and each path's delay are
 * written to @p out. With the `-w` flag, warnings are written to @p err for
 * missing delay entries, combinational loops, un-analyzed `hw.instance` ops
 * (paths are split at instance boundaries) and unlowered reactive processes.
 *
 * @param[in]  root      Operation to analyze (a module, or one `hw.module`).
 * @param[in]  costModel Cost model, with the delay side loaded (see loadFO4CostModel()).
 * @param[out] out       Stream for verbose output.
 * @param[out] err       Stream for warnings.
 * @param[in]  verbose   If true, describe each arrival time and path on @p out.
 * @return The critical path and bookkeeping; see FO4Result.
 */
FO4Result runFO4Analysis(mlir::Operation *root, const CostModel &costModel, llvm::raw_ostream &out,
                         llvm::raw_ostream &err, bool verbose);

/**
 * @brief Prints an FO4Result as a short summary report.
 *
 * Writes the critical path delay, the endpoint where it ends, each hop on the
 * path, and the number of timing paths checked.
 *
 * @param[in]  r   Result to print.
 * @param[out] out Stream to write to.
 */
void printFO4Report(const FO4Result &r, llvm::raw_ostream &out);
