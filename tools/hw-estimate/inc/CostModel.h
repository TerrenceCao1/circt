/**
 * @file CostModel.h
 * @brief Cost-model data structures and the JSON loaders that fill them.
 *
 * The cost model maps MLIR operation names to area (gate equivalents, GE) and
 * delay (fanout-of-4 inverter delays, FO4) costs. It is read from a JSON file
 * with these top-level keys:
 *
 *  - `logic_ge`        : per-op area entries (see GECost)
 *  - `delay_fo4`       : per-op delay entries (see FO4Cost)
 *  - `ff_ge_per_bit`   : GE per flip-flop bit
 *  - `sram_ge_per_bit` : GE per SRAM bit
 *  - `sram_fixed_ge`   : (optional) fixed GE per SRAM macro
 *  - `clk_to_q_fo4`    : (optional) delay added at register outputs
 *  - `setup_fo4`       : (optional) delay added at register inputs
 */

#pragma once

#include <string>
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

/**
 * @brief Area cost of one combinational operation, in gate equivalents (GE).
 *
 * The cost of an op instance is `gePerBit * width + fixedGE`, where `width` is
 * the bit width of the op's first result.
 */
struct GECost
{
  double gePerBit{};  ///< GE added per bit of result width.
  double fixedGE{};   ///< GE added per instance, independent of width.
  double quadGE{};    ///< GE that scales quadratically (mult/div)
};

/**
 * @brief Delay cost of one combinational operation, in FO4 units.
 *
 * Evaluated by evalFO4Cost(). Two models are supported:
 *  - logarithmic (when #hasLog2Model is set):
 *    `log2Coeff * log2(width) + log2Const`
 *  - linear (otherwise): `fixed + perBit * width`
 */
struct FO4Cost
{
  bool    hasLog2Model{false};      ///< True if the entry gave `fo4_log2_coeff`.
  double  log2Coeff{};              ///< Multiplier on log2(width).
  double  log2Const{};              ///< Constant added to the logarithmic model.
  double  fixed{};                  ///< Width-independent delay (linear model).
  double  perBit{};                 ///< Delay per bit of width (linear model).
  bool    scalesWithOperands{false};///< If set, variadic ops are charged as a tree of 2-input ops.
};

/**
 * @brief Complete cost model: per-op GE and FO4 tables plus sequential costs.
 *
 * Both maps are keyed by MLIR operation name (for example `"comb.add"`).
 * `comb.icmp` is split by predicate group; see #icmpPredicateGroup.
 */
struct CostModel
{
  llvm::StringMap<GECost> logicGE;                 ///< Area table, keyed by op name.
  llvm::StringMap<FO4Cost>  delayFO4;              ///< Delay table, keyed by op name (or icmp group key).
  llvm::StringMap<std::string> icmpPredicateGroup; ///< Maps an icmp predicate name (e.g. "eq") to its delayFO4 key ("comb.icmp.eq" or "comb.icmp.mag").
  double ffGEPerBit{};                             ///< GE per flip-flop bit.
  double sramGEPerBit{};                           ///< GE per SRAM bit.
  double sramFixedGE{};                            ///< GE per SRAM macro, on top of the per-bit cost.
  double clkToQFO4{};   ///< [JSON] optional "clk_to_q_fo4", added at register startpoints.
  double setupFO4{};                               ///< [JSON] optional "setup_fo4", added at register endpoints.
};

/**
 * @brief Loads the area side of the cost model from a JSON file.
 *
 * Fills CostModel::logicGE, ffGEPerBit, sramGEPerBit and sramFixedGE. Each
 * entry under `logic_ge` is read for `ge_per_bit` and `fixed_ge`.
 *
 * Terminates the process (exit code 1) if the file cannot be opened, is not
 * valid JSON, or has no `logic_ge` object. If the `-w` flag is set, missing
 * `ff_ge_per_bit` or `sram_ge_per_bit` produce a warning and default to 0.
 *
 * @bug The `ge_quadratic_k` field (used by comb.mul / comb.divu / comb.divs)
 *      is not read, so those ops currently cost 0 GE.
 * @bug Nested `comb.icmp` entries (`eq_predicates` / `mag_predicates`) are not
 *      read on the GE side, so comb.icmp currently costs 0 GE.
 *
 * @param[in,out] model Cost model to fill in. Existing entries are overwritten.
 * @param[in]     path  Path to the cost-model JSON file.
 */
void loadGECostModel(CostModel& model, llvm::StringRef path);

/**
 * @brief Loads the delay side of the cost model from a JSON file.
 *
 * Fills CostModel::delayFO4, icmpPredicateGroup, clkToQFO4 and setupFO4.
 * `comb.icmp` is special-cased: its `eq_predicates` and `mag_predicates`
 * sub-objects are stored under the keys `"comb.icmp.eq"` and
 * `"comb.icmp.mag"`, and each listed predicate name is mapped to its group.
 *
 * Terminates the process (exit code 1) if the file cannot be opened, is not
 * valid JSON, or has no `delay_fo4` object.
 *
 * @bug `fo4_log2_const` is stored in FO4Cost::fixed rather than
 *      FO4Cost::log2Const, so evalFO4Cost() ignores it for logarithmic
 *      entries.
 * @bug `fo4_per_bit` is stored in FO4Cost::fixed rather than FO4Cost::perBit,
 *      so it does not scale with width.
 * @todo Parse `scales_with_operands` into FO4Cost::scalesWithOperands.
 *
 * @param[in,out] model Cost model to fill in. Existing entries are overwritten.
 * @param[in]     path  Path to the cost-model JSON file.
 */
void loadFO4CostModel(CostModel& model, llvm::StringRef path);

/**
 * @brief Evaluates the delay of an operation of a given width.
 *
 * Uses the logarithmic model `log2Coeff * log2(max(width, 1)) + log2Const` if
 * FO4Cost::hasLog2Model is set, and `fixed + perBit * width` otherwise.
 * Variadic scaling (FO4Cost::scalesWithOperands) is applied by the caller, not
 * here.
 *
 * @param[in] cost  Delay entry for the operation.
 * @param[in] width Bit width of the operation (operand width for comb.icmp).
 * @return Delay in FO4 units.
 */
double evalFO4Cost(const FO4Cost & cost, unsigned width);
