/**
 * @file GEAnalysis.h
 * @brief Gate-equivalent (area) analysis.
 */

#pragma once

#include "CostModel.h"
#include "mlir/IR/Operation.h"
#include "llvm/Support/raw_ostream.h"

/**
 * @brief Totals produced by runGEAnalysis().
 */
struct GEResult
{
  double totalLogicGE{};        ///< GE of all combinational logic.
  double totalFFGE{};           ///< GE of all flip-flops (`totalFFBits * ffGEPerBit`).
  double totalSramGE{};         ///< GE of all SRAM (per-bit cost plus per-macro fixed cost).
  long long totalFFBits{};      ///< Number of flip-flop bits found.
  long long totalSramBits{};    ///< Number of SRAM bits found (depth * width, summed).
  long long totalSramMacros{};  ///< Number of `seq.firmem` instances found.
};

/**
 * @brief Estimates area, in gate equivalents, of everything under @p root.
 *
 * Walks the IR and handles each operation as follows:
 *  - `llhd.process`: contributes 0 GE. Reactive processes (see
 *    isReactiveLLHDProcess()) are counted and trigger a warning.
 *  - `seq.firreg`: adds its bit count to the flip-flop total.
 *  - `seq.firmem`: adds `depth * width` bits and one macro to the SRAM totals.
 *  - anything else: looked up in CostModel::logicGE and charged
 *    `gePerBit * width + fixedGE`, where `width` is the first result's integer
 *    width. Operations with no entry contribute 0 GE.
 *
 * With @p verbose, a line per contributing operation is written to @p out.
 * With the `-w` flag, warnings are reported for missing cost entries,
 * unrecognized register and memory types, and unlowered reactive processes.
 * Most warnings go to @p err; the missing-entry and unlowered-process
 * warnings currently go to @p out.
 *
 * @param[in]  root      Operation to analyze (a module, or one `hw.module`).
 * @param[in]  costModel Cost model, with the area side loaded (see loadGECostModel()).
 * @param[out] out       Stream for verbose output and some warnings.
 * @param[out] err       Stream for other warnings.
 * @param[in]  verbose   If true, describe each contribution on @p out.
 * @return Area totals; see GEResult.
 */
GEResult runGEAnalysis(mlir::Operation *root, const CostModel &costModel, llvm::raw_ostream &out,
                       llvm::raw_ostream &err, bool verbose);

/**
 * @brief Prints a GEResult as a short summary report.
 *
 * Writes the logic, flip-flop, SRAM and total GE, each to two decimal places,
 * plus the flip-flop and SRAM bit counts.
 *
 * @param[in]  r   Result to print.
 * @param[out] out Stream to write to.
 */
void printGEReport(const GEResult &r, llvm::raw_ostream &out);
