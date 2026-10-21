/**
 * @file OpClassify.h
 * @brief Small helpers for inspecting types and operations in the input IR.
 */

#pragma once

#include <cstdint>
#include "mlir/IR/Types.h"
#include "circt/Dialect/LLHD/LLHDOps.h"

/**
 * @brief Counts the total number of bits in a type.
 *
 * Integer types contribute their width. `hw.array` types contribute
 * `numElements * bits(elementType)`, recursing into nested arrays. All other
 * types (structs, clocks, and so on) are treated as having 0 bits.
 *
 * @param[in] type Type to measure.
 * @return Total bit count, or 0 for an unrecognized type.
 */
uint64_t getTotalBits(mlir::Type type);

/**
 * @brief Tells whether an `llhd.process` is reactive.
 *
 * A process is reactive if its body contains an `llhd.wait`, meaning it is
 * meant to respond to signal changes and has not been lowered to plain logic.
 * Non-reactive processes (for example `initial` blocks) are simulation-only.
 *
 * @param[in] proc Process to inspect.
 * @return True if the body contains at least one `llhd.wait`.
 */
bool isReactiveLLHDProcess(circt::llhd::ProcessOp proc);


/**
 * @brief Tells whether an operation is a not gate (since circt encodes not as xor)
 *
 * XOR gates are inverters if one input is tied high. CIRCT encodes not gates as xors
 * NOT gates have a much smaller hardware footprint than XOR gates, and thus we account
 * for this in our hw-estimate
 *
 * @param[in] op Operation to check.
 * @return True if the operation is: an XorOp, has two operands, as one input tied to 1.
 */
bool isNotOp(mlir::Operation *op);
