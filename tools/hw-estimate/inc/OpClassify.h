/**
 * @file OpClassify.h
 * @brief Small helpers for inspecting types and operations in the input IR.
 */

#pragma once

#include <cstdint>
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "mlir/IR/BuiltinTypes.h"

using namespace mlir;

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
uint64_t getTotalBits(Type type);

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
bool isNotOp(Operation *op);


/**
 * @brief Returns the non-constant operand of the NOT
 *
 * CIRCT encodes NOT gates as XOR gates with one input tied high. This helper function returns
 * the Value of the relevant operand.
 *
 * @param[in] op Parent operation.
 * @return Value of the relavant operand.
 */
Value getNotOperand(Operation *op);


/**
 * @brief Checks that this Operation is the outer not (the second NOT in a double NOT sequence).
 *
 * Checks if op is a NOT gate which has a NOT in front of it  
 *
 * @param[in] op Parent operation.
 * @return True if op is the outer gate of the double NOT.
 */
bool isDoubleInvOuter(Operation *op);

/**
 * @brief Checks that this op should be absorbed (if it feeds a single-use, 2 input gate, or if its a double not).
 *
 * Checks if op should be absorbed and thus not counted in GE or FO4.
 *
 * @param[in] op Operation checked.
 * @return True if op should be ignored, false otherwise.
 */
bool isAbsorbedNot(Operation *op);
